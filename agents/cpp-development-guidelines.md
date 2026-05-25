# C++ Development Guidelines

## General Style

- Keep public C++ code under `include/event_hub/`.
- Keep examples and tests small, readable, and focused on public behavior.
- Prefer explicit ownership, clear lifetimes, and small interfaces.
- Do not add abstractions for hypothetical future features.
- Preserve the library as header-only unless a task clearly benefits from a
  compiled implementation target.

## Event Contracts

Use direct calls for simple same-module code. Use `event-hub-cpp` when modules
should communicate without direct dependencies.

Event contracts should:

- be typed C++ structs or classes;
- have value semantics;
- avoid pointers to module internals;
- avoid global mutable state;
- stay small enough to copy or move through the queue.

## RAII Ownership

Prefer `EventEndpoint` for module-facing code. It owns subscriptions and awaiters
and releases them in its destructor.

Avoid long-lived raw owner pointers outside the bus internals. If a subscription
must outlive an endpoint, document the owner and cancellation path.

## Comments And Doxygen

Use Doxygen comments for public APIs, important concepts, and non-obvious
contracts. Keep comments compact and useful.

Avoid comments that repeat what a well-named function or type already says.

## Error And Callback Behavior

Callbacks run on the dispatching thread. Without an exception handler, callback
exceptions from dispatch are rethrown from `emit_direct<T>()` or `process()`. With
`EventBus::set_exception_handler(...)`, dispatch reports callback exceptions to
the handler and continues.

Timeout callbacks run from `poll_timeout(source) noexcept` when the source is
accepted by `AwaitOptions::delivery`; their exceptions are reported to the bus
exception handler when one is set and otherwise swallowed.

Task callbacks run on the thread that calls `TaskManager::process()`. Without
an exception handler, task exceptions are rethrown after unstarted batch work is
restored. With `TaskManager::set_exception_handler(...)`, exceptions are
reported and later tasks continue.

When changing callback or awaiter code, keep reentrancy in mind: callbacks may
subscribe, unsubscribe, post, or cancel awaiters.
