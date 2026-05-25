# Codebase Orientation

`event-hub-cpp` is a lightweight C++ event bus for modular applications. It
provides typed events, listener callbacks, queued processing, event awaiters,
task queues, event-loop helpers, and cancellation primitives.

## Expected Layout

- `include/event_hub/` contains public headers.
- `examples/` contains small focused usage examples. Add new examples to
  CMake so top-level builds compile them.
- `tests/` contains focused behavior tests.
- `agents/` contains repository instructions for AI agents.
- `temp/` is reference material supplied during project bootstrapping.

## Architecture Model

The library is intentionally small and dependency-light. Prefer clear ownership,
value-type event contracts, and explicit processing points over framework-like
abstractions.

Core vocabulary:

- `EventBus` - central bus that owns subscription lists and the queued event
  storage.
- `EventEndpoint` - RAII connection point owned by a module or service.
- `EventNode` - convenience base class for modules that combine
  `EventListener` dispatch with an owned `EventEndpoint`.
- `Event` - optional base class for users who need runtime metadata or cloning.
- `EventListener` - optional generic listener interface for `Event`-derived
  event types.
- `DeliveryPolicy` - subscription contract for direct `emit_direct()`, queued
  `process()`, or compatibility "any" delivery.
- `EventAwaiter` - cancelable helper used by `await_once()` and `await_each()`.
- `CancellationToken` and `CancellationSource` - basic cancellation primitives
  for awaiters.
- `INotifier` and `SyncNotifier` - optional external wake-up mechanism for
  application-owned event loops.
- `Task` - move-only type-erased `void()` callable for C++17 task queues.
- `TaskContext` - lightweight callback facade that lets a running task inspect
  its id, cancel future work, or request rescheduling.
- `TaskManager` - optional passive queue for immediate, delayed one-shot, and
  periodic tasks processed by an application thread.
- `CalendarScheduler` - optional `time-shield-cpp` layer that computes
  daily/weekly/monthly/custom calendar times and delegates execution to
  `TaskManager`.
- `RunLoop` - optional blocking helper that processes any number of registered
  `EventBus` and `TaskManager` instances on the calling thread.
- `Module` - `EventNode` plus an owned `TaskManager` and explicit
  initialize/process/shutdown lifecycle.
- `ModuleHub` - owner of one shared `EventBus` and a collection of modules;
  it is a passive work source with optional `run()` and `start()` wrappers.

## Event Model

Events are typed contracts. They should be small DTO-like structures with value
semantics and no ownership of module internals.

The primary API does not require inheriting from `Event`:

```cpp
struct UserLoggedIn {
    std::string name;
};

event_hub::EventBus bus;
event_hub::EventEndpoint endpoint(bus);

endpoint.subscribe<UserLoggedIn>(
    event_hub::DeliveryPolicy::queued,
    [](const UserLoggedIn& event) {
        // handle event
    });

endpoint.post<UserLoggedIn>("alice");
bus.process();
```

Use `Event` when a module needs runtime metadata, logging names, or cloning:

```cpp
class ReloadRequested final : public event_hub::Event {
public:
    EVENT_HUB_EVENT(ReloadRequested)
};
```

## EventNode Model

`EventNode` is a thin module helper, not a replacement for `EventEndpoint` or
`EventBus`. Treat it as:

```text
EventNode = EventListener + owned EventEndpoint + protected endpoint-like API
```

Use it when a module naturally wants to inherit from a base class, receive
`Event`-derived events through one `on_event(const Event&)` override, and own
its subscriptions by RAII. For plain value events, prefer
`EventEndpoint` directly unless a module already benefits from the `EventNode`
base; inside an `EventNode`, plain value events should use
`subscribe<T>(DeliveryPolicy, callback)` rather than `listen<T>(...)`. For
module-to-module traffic that must stay on the hub or run-loop thread, prefer
`subscribe_queued<T>(callback)` or `listen_queued<T>()`.

Important design points:

- `EventNode` inherits from `EventListener` so derived modules can call
  `listen<EventType>(DeliveryPolicy::queued)` or a named helper such as
  `listen_queued<EventType>()` and dispatch inside `on_event()` with
  `Event::as<T>()` or `Event::as_ref<T>()`.
- `EventNode` owns an internal `EventEndpoint`; all subscription, `post`,
  `emit_direct`, and unsubscribe behavior must delegate to that endpoint.
- `listen<T>(DeliveryPolicy)` is only for types derived from
  `event_hub::Event`. Keep the static assertion so misuse fails at compile time
  with a clear message.
- The event API on `EventNode` is protected on purpose. External code should
  use the module's business methods, not treat the module object as a public
  event bus facade.
- Public `EventNode` API should stay limited to construction/destruction,
  `close()`, `is_closed()`, and `bus()`.
- `close()` delegates to `EventEndpoint::close()` and must remain idempotent.
  Closing expires the endpoint guard, cancels awaiters owned by the endpoint,
  and unsubscribes endpoint-owned subscriptions.
- Destruction closes the internal endpoint. This prevents new guarded callbacks
  from starting after close/destruction, but does not wait for callbacks that
  have already started.
- Modules managed by `std::shared_ptr` may still need an explicit
  `weak_from_this()` guard with
  `subscribe<T>(DeliveryPolicy, guard, callback)` when callbacks capture module
  state and strict lifetime is required.
- Do not add task systems, thread pools, coroutine drain models,
  `close_and_wait()`, or bus architecture changes to support `EventNode`.

## Subscription Lifetime Guards

The bus intentionally copies callback records under `m_subscriptions_mutex`,
releases the mutex, and only then invokes callbacks. This avoids deadlocks and
allows callbacks to subscribe, unsubscribe, post, or cancel awaiters while an
event is being dispatched. The tradeoff is that `unsubscribe()` and
`unsubscribe_all()` remove future records from the bus, but they cannot erase
records already copied into an active `dispatch()` call.

To make that model safe for endpoint-owned modules, subscriptions support a
lifetime guard in addition to the raw owner pointer:

- `CallbackRecord::owner` remains a non-owning key for targeted
  `unsubscribe_all(owner)` and `unsubscribe_all<EventType>(owner)`.
- `CallbackRecord::has_guard` distinguishes an intentional empty guard from an
  unguarded subscription.
- `CallbackRecord::guard` is a `std::weak_ptr<void>` checked immediately before
  callback invocation.
- `EventBus::next_subscription_id()` uses an atomic counter so concurrent
  `subscribe()` calls can produce unique ids.

In `dispatch()`, guarded callbacks must use `guard.lock()`, not only
`expired()`. The resulting `std::shared_ptr<void> alive` is held in a local
variable until the callback returns. This gives the precise guarantee:

- if the guard has already expired before the callback starts, the callback is
  skipped;
- if the callback starts, the guard object is retained until that callback
  returns;
- callbacks that already started are not stopped, interrupted, drained, or
  waited for by `unsubscribe_all()` or `close()`.

`EventEndpoint` is the default source of guards. It owns `m_guard`, passes
`guard()` to every endpoint subscription, and resets `m_guard` during
`close()`. That reset prevents any later copied endpoint callback from starting.
The endpoint still keeps `owner == this` on records so explicit
`unsubscribe(id)`, `unsubscribe<EventType>()`, and `unsubscribe_all()` continue
to remove records from the bus storage.

There are two guard layers by design:

- Endpoint guard: `endpoint.subscribe<T>(DeliveryPolicy, callback)` protects
  callbacks from starting after the endpoint is closed or destroyed.
- User guard:
  `endpoint.subscribe<T>(DeliveryPolicy, weak_from_this(), callback)` adds a
  module/object lifetime guard for `std::shared_ptr`-managed modules.

The user guard overload is intentionally implemented as an additional check
inside the callback wrapped by `EventEndpoint`, while the bus-level record still
uses the endpoint guard. This means both conditions must hold: the endpoint must
still be alive at dispatch start, and the user guard must lock before user code
runs. When strict module lifetime matters, prefer capturing the weak pointer and
locking it inside the callback before touching module state:

```cpp
auto weak = weak_from_this();

m_endpoint.subscribe_queued<TokenFoundEvent>(
    weak,
    [weak](const TokenFoundEvent& event) {
        if (auto self = weak.lock()) {
            self->on_token_found(event);
        }
    });
```

Awaiters follow the same ownership idea. Awaiters created through
`EventEndpoint` receive the endpoint guard, and `EventAwaiter` also uses
`weak_from_this()` internally so cancelled or destroyed awaiters do not run
stale callbacks.

Keep this as a cooperative lifetime model. Do not replace it with blocking
callback drains, thread joins, task systems, or `close_and_wait()` unless a task
explicitly changes the library's threading contract.

## Dispatch Rules

- `emit_direct<T>()` dispatches synchronously on the calling thread.
- `post<T>()` enqueues an event for later dispatch.
- `subscribe<T>()` requires an explicit `DeliveryPolicy`.
- `subscribe_any<T>()` uses `DeliveryPolicy::any` for backward compatibility.
- `subscribe_direct<T>()` receives only `emit_direct<T>()` delivery.
- `subscribe_queued<T>()` receives only queued `process()` delivery.
- `emit_direct<T>()` returns `DispatchResult` with matched, delivered, and
  policy-skipped subscription counts.
- `set_delivery_mismatch_handler(...)` reports policy mismatches. The default
  `DeliveryMismatchReportMode::no_delivery` reports only when all subscribers
  were policy-skipped; `any_skipped` is available for verbose mixed-policy
  diagnostics.
- Prefer queued subscriptions for module state with hub-thread or run-loop
  affinity. Use direct subscriptions only when callbacks are thread-safe and
  reentrancy-safe.
- If an `INotifier` is configured, `post<T>()` calls `notify()` after the event
  is queued.
- `process()` drains the queue snapshot taken at the start of the call.
- Events posted by handlers are processed by a later `process()` call.
- `clear_pending()` clears only the async queue, not subscriptions.
- Subscribers do not know concrete publishers.
- `EventEndpoint` unsubscribes and cancels its awaiters on destruction.
- `EventEndpoint` subscriptions carry a lifetime guard; callbacks copied by an
  active dispatch are skipped when the guard has expired before callback start.
- Awaiters use queued delivery by default. `AwaitOptions::delivery` filters
  event callbacks, timeout callbacks, and cancellation cleanup. Queued awaiters
  observe timeout and cancellation only at queued poll points; call `cancel()`
  directly for immediate cross-thread cancellation. Set delivery to
  `DeliveryPolicy::direct` or `DeliveryPolicy::any` only when those paths may
  safely run from an `emit_direct<T>()` caller thread.
- `unsubscribe_all()` does not wait for callbacks that already started or
  already passed the guard check.
- Modules that need strict object lifetime during callbacks should be owned by
  `std::shared_ptr`, derive from `std::enable_shared_from_this`, and pass
  `weak_from_this()` as an explicit subscription guard.
- Without an exception handler, dispatch rethrows callback exceptions. With
  `set_exception_handler(...)`, dispatch reports them and continues.
- Event handlers should be short; heavy work should schedule a task or publish
  a follow-up event.

## TaskManager Model

`TaskManager` is deliberately parallel to `EventBus`, not a replacement for it.
Use events for typed module contracts and tasks for ordinary `void()` work that
must run at explicit processing points.

Important design points:

- `TaskManager` is passive and header-only. It does not own a thread, sleep by
  itself, or expose `run()`/`join()` APIs.
- Producers may call `post`, `post_after`, `post_at`, `add_task_at`,
  `post_every`, `post_every_after`, `post_batch`, `submit`, and `cancel` from
  other threads. `process`, `clear_pending`, `close`, and destruction remain
  single-consumer/lifetime operations.
- `post_at(...)` and `add_task_at(...)` schedule against
  `TaskManager::TimePoint`, a `std::chrono::steady_clock::time_point`. Treat it
  as a monotonic deadline for retry, timeout, reconnect, polling, debounce, and
  `next_deadline()` loops; do not describe it as a wall-clock date/time API.
- `add_task_at_system(...)` accepts `std::chrono::system_clock::time_point` and
  converts it once to a steady deadline at scheduling time. It does not move if
  the system clock changes later. `add_task_at_system_ms(...)` is the same API
  shape for Unix epoch milliseconds.
- Calendar rules such as daily, weekly, monthly, or timezone-aware schedules
  live in the optional `CalendarScheduler` layer over `TaskManager`, backed by
  `time-shield-cpp` only when `EVENT_HUB_CPP_USE_TIME_SHIELD=ON`. It computes
  UTC Unix-millisecond planned times, then queues one steady-clock task at a
  time.
- `Task` exists because C++17 `std::function<void()>` cannot store move-only
  callables. It also supports `void(TaskContext&)` callbacks for self-cancel
  and reschedule. Keep this wrapper small and dependency-free.
- `TaskContext` is an opt-in callback facade, not an owning handle. It is valid
  only during the callback. Use it when a task needs `id()`, `cancel()`, or
  `reschedule_at/after()`; keep simple examples on no-arg callbacks.
- Context-aware callbacks are supported by `post`, delayed one-shot APIs,
  periodic APIs, and `post_batch` tasks already constructed as `Task`.
  `submit()` deliberately remains no-context because it is a future-result API.
- Context reschedule keeps the same `TaskId`. For one-shot tasks it reruns the
  same callback; for periodic tasks it overrides only the next cycle, then the
  normal fixed-delay or fixed-rate policy resumes. If cancellation and
  reschedule are both requested inside one callback, cancellation wins.
- Ready queues are fixed high/normal/low FIFO deques. Delayed and periodic
  tasks use `std::chrono::steady_clock` deadlines in a min-heap.
- Periodic tasks keep one `TaskId` for the whole series. The first cycle is
  ready immediately by default; later cycles use fixed-delay or fixed-rate
  scheduling according to `PeriodicTaskOptions`.
- Cancellation is lazy through `TaskId` and atomic state. Do not make eager
  container erase the main cancellation mechanism.
- `process()` takes a snapshot of ready work. Tasks posted by callbacks run on
  a later `process()` call. User callbacks never run while the queue mutex is
  held.
- Exception behavior matches `EventBus`: without a handler, rethrow and restore
  unstarted work from the current batch; with a handler, report and continue.

`RunLoop` is an optional convenience wrapper for applications that want a
blocking current-thread loop. It owns one `SyncNotifier`, registers it on added
sources, processes all registered buses and task managers, and blocks until
`request_stop()` or a user predicate ends the loop. Registered sources are
non-owning and must outlive the loop.

## Module Layer

Use `Module` and `ModuleHub` when an application should be composed from
independent components sharing one typed bus. Each module owns its own
`TaskManager`; the hub owns the shared `EventBus` and module collection.

`ModuleHub` must remain a passive source of work. `process()`, `has_pending()`,
`next_deadline()`, and notifier wiring are first-class APIs for embedding into
external loops. `run()` and `start()` are convenience wrappers over the same
processing model.

For module-layer changes, read `agents/modules-and-module-hub.md` before
editing public APIs, examples, or tests.

## Threading Model

Subscription storage and the async queue are protected by mutexes. `post<T>()`
is safe to call from producer threads. Dispatch happens on the thread that calls
`emit_direct<T>()` or `process()`, filtered by each subscription's `DeliveryPolicy`.

Prefer calling `process()`, `emit_direct()`, `subscribe()`, and `unsubscribe()` from the
application/event-loop thread unless the application provides its own stronger
synchronization.

`EventBus` and `TaskManager` must not own threads or sleep on their own. For
efficient external loops, attach a non-owning `INotifier` with
`set_notifier(...)`. A source only signals "pending work may exist"; the
application decides whether to wait on a condition variable, sleep until a
delayed task deadline, process a task queue, handle timers, or combine several
wake-up sources. `RunLoop` is the optional helper for that blocking loop, but it
still runs on the caller's thread.

Prefer the external notifier model over a condition variable hidden inside
`EventBus`, because one application thread often needs a single wake-up source
shared by the bus, task queues, timer queues, and UI or framework adapters.
`SyncNotifier` uses a generation counter so an event posted after the loop reads
`generation()` but before it calls `wait_for(...)` is not lost; the wait returns
immediately when it observes that the generation changed.
