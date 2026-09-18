# Changelog

All notable changes to this project will be documented in this file.

## [v0.1.0] - 2026-09-19
- Changed the direct event dispatch API from `emit<T>()` to
  `emit_direct<T>()`; the old name is intentionally not kept as an alias.
- Added `DispatchResult` return statistics for direct dispatch and an optional
  delivery mismatch handler for policy-skipped subscribers. The handler defaults
  to no-delivery-only reporting and can opt into any-skipped diagnostics.
- Added `Module` and `ModuleHub` for composing applications from modules that
  share one `EventBus` while each module owns its own `TaskManager`.
- Added module execution modes for inline hub processing, private module
  worker threads, and externally processed manual task queues.
- Added passive `ModuleHub` integration APIs, including `process()`,
  `process_once()`, `has_pending()`, `next_deadline()`, notifier wiring, and
  cooperative stop handling, plus active `run()`, `start()`, and `join()`
  wrappers.
- Added module-focused examples for blocking hub execution, background hub
  execution, and embedding a hub into an existing external loop.
- Added module behavior tests for lifecycle order, per-module task queues,
  inline fairness, manual queues, private workers, notifier wiring, deadlines,
  active-loop lifecycle, validation, initialization cleanup, and exception
  propagation.
- Documented the module architecture and design rationale for AI agents under
  `agents/modules-and-module-hub.md`.
- Fixed a `ModuleHub::start()` race where an early `request_stop()` could be
  cleared by background initialization.
- Added periodic `TaskManager` scheduling with fixed-delay and fixed-rate
  policies, optional initial delay, cancellation through `TaskId`, and tests.
- Added `TaskContext` callbacks for task self-cancel and explicit reschedule.
- Added `TaskManager::add_task_at(...)` as an explicit steady-clock deadline
  alias for `post_at(...)`.
- Added `TaskManager::add_task_at_system(...)` for one-shot wall-clock
  submissions that are converted to steady deadlines at scheduling time.
- Added `TaskManager::add_task_at_system_ms(...)` for one-shot wall-clock
  submissions represented as Unix epoch milliseconds.
- Added optional `time-shield-cpp` submodule integration for calendar-time
  scheduling utilities, disabled by default.
- Added optional `CalendarScheduler` daily, weekly, monthly, and custom
  calendar rules over `TaskManager` when `EVENT_HUB_CPP_USE_TIME_SHIELD=ON`.
- Added `CalendarTaskOptions` helpers for named zones, fixed UTC offsets,
  caller-provided `time_shield::ZonedClock` instances, and custom UTC
  millisecond providers.
- Added CalendarScheduler examples for NTP-backed time and custom time around a
  winter-time transition.

- Added the initial header-only C++17 event bus API with typed subscriptions,
  synchronous `emit<T>()`, queued `post<T>()`, explicit `process()`, pending
  queue inspection, and queue clearing.
- Added RAII subscription ownership through `EventEndpoint`, including endpoint
  lifetime guards, targeted unsubscribe helpers, and guarded callback dispatch.
- Added optional `Event`/`EventListener` contracts for runtime event metadata,
  safe downcasts, cloning, and listener-style dispatch.
- Added `EventNode` as a convenience base for modules that combine
  `EventListener` dispatch with an owned endpoint.
- Added cancelable event awaiters with `await_once()`, `await_each()`,
  `CancellationToken`, `CancellationSource`, timeout polling, and timeout
  callback error reporting.
- Added `Task`, `TaskManager`, task priorities, delayed one-shot tasks,
  `submit()` futures, cancellation, batch posting, and exception handling for
  explicit application-owned task processing.
- Added `INotifier`, `SyncNotifier`, and `RunLoop` for integrating event buses
  and task managers with current-thread application run loops.
- Added focused examples for basic events, event/listener usage, `EventNode`,
  awaiters and cancellation, exception handling, strict lifetime guards,
  external notifiers, task queues, task/event-bus integration, and `RunLoop`.
- Added focused behavior coverage for event dispatch, lifetime guards, awaiters,
  cancellation, task management, notifier behavior, run-loop processing, and
  exception policies.
- Added CMake interface target `event_hub::event_hub`, install/export metadata,
  `find_package(event-hub-cpp CONFIG REQUIRED)` support, package-name alias
  `event-hub-cpp::event-hub-cpp`, and install-consumer coverage.
- Added pkg-config generation, a vcpkg overlay port, and CI coverage for
  Windows, Linux, macOS, C++17, C++20, sanitizer checks, installed-package
  consumption, and vcpkg overlay validation.
- Added README and README-RU documentation with package-manager integration
  notes, platform/status badges, tested-platform tables, examples, threading
  model notes, and integration guidance.
