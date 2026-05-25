# Request/Response And Reply Guide

Use this guide when adding request-style workflows to `event-hub-cpp` users or
examples.

## Preferred Pattern: Request/Result Events

Prefer paired request/result event types when a caller needs one concrete answer
for one concrete request:

```cpp
struct ProxyCheckRequest {
    event_hub::RequestId request_id = event_hub::invalid_request_id;
    Proxy proxy;
};

struct ProxyCheckResult {
    event_hub::RequestId request_id = event_hub::invalid_request_id;
    bool ok = false;
};
```

Callers should use `EventEndpoint::request<RequestEvent, ResultEvent>()` or the
protected `EventNode`/`Module` forwarding methods:

```cpp
endpoint.request<ProxyCheckRequest, ProxyCheckResult>(
    request,
    [](const ProxyCheckResult& result) {
        // handle result
    });
```

The helper assigns a bus-wide `RequestId`, stores it in the request via
`RequestTraits<RequestEvent>::set_id`, installs an `await_once<ResultEvent>()`
filtered by the same id, and posts the request event.

Use `request_future<RequestEvent, ResultEvent>()` only when a future-based API
is clearer for the caller. Remember that the library is passive: someone still
must call `emit_direct()` or `process()` to dispatch events and poll awaiter timeouts.

## Custom Correlation Field

If events use a different field name, specialize `RequestTraits<T>`:

```cpp
namespace event_hub {

template <>
struct RequestTraits<MyResult> {
    static RequestId get_id(const MyResult& event) noexcept {
        return event.correlation_id;
    }

    static void set_id(MyResult& event, RequestId id) noexcept {
        event.correlation_id = id;
    }
};

} // namespace event_hub
```

Keep the correlation id on both request and result events. Do not use hidden
module pointers or direct service references for result routing.

## Queue Snapshot Rule

`EventBus::process()` drains the queue snapshot captured at the start of the
call. If a request handler posts a result event, that result is processed by a
later `process()` call, not the same one:

```cpp
bus.process(); // handles request and posts result
bus.process(); // handles result and invokes request callback
```

Examples and tests should make this behavior explicit.

## Reply<T> Shortcut

`Reply<T>` is a copyable callback wrapper for callback-in-event request
shortcuts:

```cpp
struct BalanceRequest {
    std::string account;
    event_hub::Reply<BalanceResult> reply;
};
```

This is acceptable for tightly coupled flows where the response is only for the
sender and does not need to be observed by other modules.

Do not use `Reply<T>` as the default architecture. Prefer request/result events
when responses should be logged, filtered, replayed, awaited, tested as normal
events, or consumed by more than one module.

## Module And EventNode

`Module` inherits from `EventNode`, so request helpers should be exposed on
`EventNode` and automatically available to `Module` subclasses. Avoid adding a
second copy of the same forwarding methods directly to `Module` unless the
module layer needs different semantics.

## Design Boundaries

- Keep request helpers as a thin layer over typed events and `await_once()`.
- Do not turn `EventBus` into an RPC framework or thread-owning runtime.
- Do not add blocking waits inside `EventBus`, `EventEndpoint`, `EventNode`, or
  `Module`.
- Preserve passive processing: applications decide when to call `process()`.
- Prefer value-type DTO events with public correlation fields.
