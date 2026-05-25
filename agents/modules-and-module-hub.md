# Modules And ModuleHub

`Module` and `ModuleHub` are the generic application-composition layer on top
of the core event and task primitives. Treat this document as canonical; notes
under `temp/deep-research-report.md` are useful background but may describe an
older snapshot of the code.

## Purpose

Use modules when an application should be built from independent parts that
communicate through typed events:

- parsers, bots, API adapters, pollers, or workflow services;
- components that need their own task queue;
- components that benefit from explicit initialize/process/shutdown lifecycle.

Do not use `ModuleHub` as a replacement for `EventBus`, `TaskManager`, or
`RunLoop` in small examples where direct composition is clearer.

## Architecture

`Module` is:

```text
Module = EventNode + owned TaskManager + lifecycle + optional private worker
```

Each module owns exactly one `TaskManager`. This is intentional:

- isolation: one module cannot fill another module's task queue;
- fairness: `ModuleHub` can process inline modules with per-module quotas;
- migration path: a module can move from inline execution to a private worker
  without changing its event contracts;
- ownership clarity: tasks belong to the module that scheduled them.

`ModuleHub` is:

```text
ModuleHub = one shared EventBus + owned modules + passive processing API
```

The hub owns the shared `EventBus` and `std::unique_ptr<Module>` collection. It
initializes modules in registration order and shuts them down in reverse order.
Keep module registration before `initialize()`, `run()`, or `start()`; the
module collection is not synchronized with processing.

## Execution Modes

Every module has one `ModuleExecutionMode`:

- `inline_in_hub` - the module task manager is processed by
  `ModuleHub::process()` using `max_tasks_per_process`.
- `private_thread` - the module owns a private worker thread that processes
  only that module's task manager. It still communicates through the hub's
  shared `EventBus`.
- `manual` - the module owns a task manager, but neither the hub nor a private
  module worker processes it. External code must call `module.tasks().process()`
  and wire notifiers when needed.

Prefer `inline_in_hub` by default. Use `private_thread` for work that should
progress independently of the hub thread. Use `manual` only for integration
with a foreign loop or scheduler.

## Lifecycle

`Module` exposes public lifecycle methods and protected hooks:

- `initialize()` calls `on_initialize()` once.
- `process()` calls `on_process()` for lightweight main-loop/integration work.
- `shutdown()` stops the private worker, calls `on_shutdown()`, closes tasks,
  clears pending tasks, and closes the inherited `EventNode` endpoint.

Active hub modes manage lifecycle automatically:

- `ModuleHub::run()` initializes, blocks on the current thread, then shuts down.
- `ModuleHub::start()` initializes and runs on a background thread; `join()`
  waits and rethrows stored background exceptions.

Passive integration stays explicit:

```cpp
event_hub::ModuleHub hub;
hub.emplace_module<MyModule>();

hub.initialize();
while (running) {
    hub.process();
}
hub.shutdown();
```

This explicit passive path is important for GUI loops, service loops, tests,
and applications that already own a main loop.

## Passive Source Contract

`ModuleHub` is a passive work source first. Do not design features that require
the hub to own a thread.

Use these APIs for embedding:

- `process()` / `process_once()` - one non-blocking pass;
- `has_pending()` - immediate processable work exists;
- `next_deadline()` - earliest future inline task or module deadline hint;
- `set_notifier(INotifier*)` / `reset_notifier()` - non-owning wake-up source;
- `request_stop()` - cooperative stop signal for active loops and private
  module workers;
- `join()` - wait for a background `start()` loop.

`has_pending()` intentionally means "work can run now", not "a future delayed
task exists". Use `next_deadline()` to wait efficiently for delayed work.

Manual external loops should follow the same lost-wakeup-safe pattern as
`RunLoop`: capture notifier generation before processing all sources, then wait
on that old generation only after all sources are idle.

## Event And Task Boundaries

Use the shared `EventBus` for module-to-module contracts. Events should remain
small value-type DTOs and should not contain pointers to module internals.
When a module callback assumes hub-thread or run-loop affinity, subscribe with
`subscribe_queued<T>(...)` or `listen_queued<T>()`. The base
`subscribe<T>(DeliveryPolicy, ...)` requires an explicit policy; use
`subscribe_any<T>()` only when the callback is thread-safe and reentrancy-safe
for both direct `emit()` and queued `process()` delivery.

Use a module's `TaskManager` for private `void()` work that must run at explicit
processing points. Heavy event handlers should enqueue tasks or post follow-up
events rather than doing long work inside dispatch.

Private-thread modules must still treat the shared bus as the communication
boundary. Their worker should process only their own `TaskManager`; it should
not drain the hub bus or another module's tasks.

## What Not To Copy From BaseTradingPlatform

The old `temp/BaseTradingPlatform.hpp` design mixed domain facade, module
registry, event bus host, lifecycle root, and task loop host in one class. Do
not reproduce that in this generic library layer.

Avoid these patterns:

- lifecycle encoded as magic named tasks such as `initialize` or `loop`;
- mandatory 1 ms heartbeat loops;
- one shared task manager for every module;
- raw module pointer ownership;
- hidden mandatory background threads;
- combining domain APIs with generic orchestration.

The selected architecture keeps the core dependency-light and lets applications
choose whether the hub is processed by an outer loop, a blocking call, or a
background thread wrapper.

## Testing Expectations

Module-related changes should cover:

- initialization order and reverse shutdown order;
- one task manager per module;
- inline quota/fairness behavior;
- manual mode not being processed by the hub;
- private worker task processing and event publication through the shared bus;
- passive `process()` snapshot behavior;
- `has_pending()`, `next_deadline()`, and notifier wakeups;
- `run()`, `start()`, `request_stop()`, and `join()` lifecycle;
- exception propagation from hub processing and private module workers.

Use examples to show distinct integration styles instead of one oversized
example: blocking `run()`, background `start()/join()`, and embedding
`ModuleHub` into an existing external loop.
