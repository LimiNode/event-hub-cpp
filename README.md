# event-hub-cpp

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-blue.svg)](.github/workflows/ci.yml)
[![Language](https://img.shields.io/badge/language-C%2B%2B17%2B-orange.svg)](CMakeLists.txt)
[![Header only](https://img.shields.io/badge/header--only-yes-brightgreen.svg)](include/event_hub.hpp)
[![Packages](https://img.shields.io/badge/packages-CMake%20%7C%20pkg--config%20%7C%20vcpkg%20overlay-6f42c1.svg)](#installation)
![CI Windows](https://img.shields.io/github/actions/workflow/status/NewYaroslav/event-hub-cpp/ci.yml?branch=main&label=Windows&logo=windows)
![CI Linux](https://img.shields.io/github/actions/workflow/status/NewYaroslav/event-hub-cpp/ci.yml?branch=main&label=Linux&logo=linux)
![CI macOS](https://img.shields.io/github/actions/workflow/status/NewYaroslav/event-hub-cpp/ci.yml?branch=main&label=macOS&logo=apple)

[Read in Russian](README-RU.md)

## Overview

`event-hub-cpp` is a lightweight C++ event bus for modular applications. It
provides typed events, RAII-owned subscriptions, synchronous dispatch, queued
processing, awaiters, and basic cancellation primitives.

The library is useful when modules need to publish commands, notifications, or
state changes without depending on each other's concrete implementations.

Key characteristics:

- Typed event contracts with callbacks such as `void(const MyEvent&)`.
- `EventEndpoint` owns subscriptions and unsubscribes automatically.
- `EventNode` offers a protected endpoint-like API for listener-style modules.
- `emit<T>()` dispatches synchronously.
- `post<T>()` queues work for an explicit `process()` point.
- `subscribe<T>(DeliveryPolicy, ...)` makes callback thread affinity explicit;
  `subscribe_any<T>()` keeps the compatibility "both" behavior.
- Optional `TaskManager` queues immediate, delayed, periodic, and
  future-returning tasks for the same explicit processing model.
- Optional `CalendarScheduler` adds daily, weekly, monthly, and custom
  calendar rules when `time-shield-cpp` integration is enabled.
- Optional `RunLoop` blocks the calling thread while processing any number of
  event buses and task managers.
- Optional external notifiers wake application event loops after queued work is
  posted.
- `await_once()` and `await_each()` provide callback-based waiting.
- `CancellationToken` and `CancellationSource` cancel awaiters.
- Lifetime guards prevent callbacks from starting after their guarded owner
  expires.
- Header-only C++17 with no required external dependencies.

## Recommended Project Structure

For larger applications, prefer a module-oriented layout:

```text
src/<module>.hpp
src/<module>/module.hpp
src/<module>/events.hpp
src/<module>/data/
src/<module>/providers/
src/<module>/adapters/
src/<module>/detail/
```

`data/` contains pure DTOs and configs. `events.hpp` contains event-bus
contracts. `module.hpp` is the component entry point used by composition roots.
`providers/` contains backend implementations. `adapters/` contains bridges to
event bus, HTTP, CLI, Telegram, WebSocket, or other transports.

Module-level `module.hpp` files should document the module's event API with
Doxygen comments: accepted request events, published result/notification events,
request/result correlation, and processing assumptions.

See [docs/project-structure.md](docs/project-structure.md) for the full guide.

## Header Layout

| Header | Purpose |
| --- | --- |
| `<event_hub.hpp>` | Primary umbrella header for the dependency-free public API. |
| `<event_hub/event_bus.hpp>` | Central bus, subscriptions, `emit`, `post`, and `process`. |
| `<event_hub/event_endpoint.hpp>` | RAII module endpoint for subscriptions and awaiters. |
| `<event_hub/event_node.hpp>` | Convenience base class for modules that own an endpoint and listen through `EventListener`. |
| `<event_hub/event_awaiter.hpp>` | Awaiter implementation and `AwaitOptions`. |
| `<event_hub/awaiter_interfaces.hpp>` | Cancelable awaiter handle interfaces. |
| `<event_hub/cancellation.hpp>` | Cancellation token/source primitives. |
| `<event_hub/event.hpp>` | Optional base event contract with type/name/clone metadata. |
| `<event_hub/event_listener.hpp>` | Optional generic listener interface for `Event`-derived types. |
| `<event_hub/notifier.hpp>` | `INotifier` and `SyncNotifier` for external event-loop wakeups. |
| `<event_hub/run_loop.hpp>` | Blocking current-thread loop for registered buses and task managers. |
| `<event_hub/task.hpp>` | Move-only `Task`, `TaskContext`, `TaskId`, priority, periodic policy, and task options. |
| `<event_hub/task_manager.hpp>` | Passive `TaskManager` for immediate, delayed, and periodic task processing. |
| `<event_hub/calendar_scheduler.hpp>` | Optional `time-shield-cpp` calendar scheduler layer for daily/weekly/monthly rules. |

## Quick Start

```cpp
#include <event_hub.hpp>

#include <iostream>
#include <string>

struct MyEvent {
    std::string message;
};

int main() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint endpoint(bus);

    endpoint.subscribe<MyEvent>(
        event_hub::DeliveryPolicy::queued,
        [](const MyEvent& event) {
            std::cout << event.message << '\n';
        });

    endpoint.post<MyEvent>("sent from the queue");
    bus.process();

    endpoint.subscribe_direct<MyEvent>([](const MyEvent& event) {
        std::cout << event.message << '\n';
    });
    endpoint.emit<MyEvent>("sent immediately");
}
```

Events can be plain C++ value types. Derive from `event_hub::Event` only when
you need runtime metadata or cloning:

```cpp
struct TokenFoundEvent {
    std::string source;
    std::string value;
};
```

```cpp
class ReloadRequested final : public event_hub::Event {
public:
    EVENT_HUB_EVENT(ReloadRequested)
};
```

## EventNode

`EventNode` is a convenience base for modules that want `EventListener`
dispatch and RAII-owned subscriptions in one object. It owns an internal
`EventEndpoint`, exposes `listen`, `subscribe`, `post`, `emit`, and
`unsubscribe` only to derived classes, and closes the endpoint on destruction.

```cpp
class ReloadModule final : public event_hub::EventNode {
public:
    explicit ReloadModule(event_hub::EventBus& bus)
        : EventNode(bus) {}

    void start() {
        listen_queued<ReloadRequested>();
    }

    void request_reload() {
        post<ReloadRequested>();
    }

    void on_event(const event_hub::Event& event) override {
        if (event.is<ReloadRequested>()) {
            // reload settings
        }
    }
};
```

Use `listen<T>(DeliveryPolicy)` for `event_hub::Event`-derived events handled
through `on_event(const Event&)`. For plain value events, use
`subscribe<T>(DeliveryPolicy, callback)`. For module-to-module traffic that
must run only from the hub or run-loop processing thread, prefer
`listen_queued<T>()` or `subscribe_queued<T>(callback)`. Use `listen_any<T>()`
or `subscribe_any<T>()` only when a callback is intentionally safe for both
delivery sources. Modules owned by `std::shared_ptr` can pass
`weak_from_this()` to `subscribe<T>(DeliveryPolicy, guard, callback)` when
callbacks capture module state.

## Awaiters

`await_once()` listens until the first matching event and then cancels itself.
`await_each()` keeps listening until its handle, endpoint, timeout, or
cancellation token stops it.

```cpp
event_hub::CancellationSource source;

auto options = event_hub::AwaitOptions::timeout_ms(5000);
options.token = source.token();
options.on_timeout = [] {
    // handle timeout
};

endpoint.await_once<MyEvent>(
    [](const MyEvent& event) { return event.message == "ready"; },
    [](const MyEvent& event) {
        // handle first matching event
    },
    options);

auto stream = endpoint.await_each<MyEvent>([](const MyEvent& event) {
    // handle every event
});

stream->cancel();
```

Timeouts and external cancellation are polled by `emit<T>()` and `process()`.
Awaiters use queued delivery by default. Set
`options.set_delivery(event_hub::DeliveryPolicy::direct)` or
`DeliveryPolicy::any` when an awaiter is intentionally allowed to complete from
an `emit<T>()` caller thread.

## TaskManager

`TaskManager` is an optional passive work queue for applications that want to
process non-event tasks from the same external loop as `EventBus`. It accepts
work from producer threads, but callbacks run only when the application calls
`process()`.

```cpp
event_hub::TaskManager tasks;

tasks.post([] {
    // run on the thread that calls tasks.process()
});

auto future = tasks.submit([] {
    return 42;
});

tasks.process();
assert(future.get() == 42);
```

Tasks can be immediate, one-shot delayed, or periodic, and ready tasks can use
fixed `high`, `normal`, or `low` priorities while preserving FIFO order inside
each priority. `Task` is move-only, so C++17 code can queue lambdas that capture
move-only values.

For a monotonic absolute deadline, use `add_task_at(...)` or `post_at(...)` with
`TaskManager::TimePoint` (`std::chrono::steady_clock::time_point`). This is not
a calendar date; it is stable against system clock and DST changes, and works
well with `next_deadline()`.

For a simple wall-clock submission, use `add_task_at_system(...)` with
`std::chrono::system_clock::time_point`. It converts the system time to a
steady-clock deadline at scheduling time and does not track later system clock
changes, so it is not a full calendar scheduler. Use
`add_task_at_system_ms(...)` when the same wall-clock deadline is represented as
Unix epoch milliseconds.

Periodic tasks run only from `process()`. By default, the first cycle is ready
immediately, then later cycles use fixed-delay scheduling. Use
`post_every_after(...)` or `post_every_after_ms(...)` to delay the first cycle,
and set `PeriodicTaskOptions::schedule` to `PeriodicSchedule::fixed_rate` when
cycles should be based on planned deadlines instead of callback completion time.
Cancel the returned `TaskId` to stop future cycles. If a periodic callback
throws, that periodic task stops and the normal `TaskManager` exception policy
applies.

Task callbacks may also accept `event_hub::TaskContext&`. The context exposes
the running task id, `cancel()`, and `reschedule_at(...)` /
`reschedule_after(...)`, which is useful for self-cancelling periodic work or
retry-like one-shot tasks without capturing a `TaskId`.

When configured with `-DEVENT_HUB_CPP_USE_TIME_SHIELD=ON`, the optional
`<event_hub/calendar_scheduler.hpp>` header adds daily, weekly, monthly, and
custom calendar rules over `TaskManager`. It computes the next UTC calendar occurrence with
`time-shield-cpp`, submits one steady-clock task, and schedules the next one
after the callback completes. The default zone is UTC/GMT; options provide a
`time_shield::ZonedClock`, a UTC millisecond `now_provider`, missed-run and
overlap policies, persisted `last_due_utc_ms`, and observer callbacks for
created/scheduled/due/callback/missed/cancelled events. Convenience helpers
such as `CalendarTaskOptions::in_zone(...)`,
`CalendarTaskOptions::fixed_utc_offset(...)`, and
`CalendarTaskOptions::with_clock(...)` configure common clock sources. To use
time-shield NTP, pass a `time_shield::ZonedClock(zone, true)` in an
NTP-enabled build. For named zones, local times that fall into DST gaps are
skipped, and ambiguous DST-fold local times are scheduled once at the first UTC
occurrence.

Use `time_of_day(...)` for readable wall-clock times and fluent schedule
builders for multi-day rules. It creates a time inside the configured calendar
day, not the host machine's current local time. CalendarScheduler uses second
granularity for daily/weekly/monthly rules, so a nonzero millisecond argument
is rejected by those APIs:

```cpp
calendar.add_daily_task(event_hub::time_of_day(18, 30), [] {});

auto weekly = event_hub::WeeklySchedule{}
    .add(event_hub::MON, event_hub::time_of_day(9, 0))
    .add(event_hub::FRI, event_hub::time_of_day(18, 0));
calendar.add_weekly_task(std::move(weekly), [] {});

auto monthly = event_hub::MonthlySchedule{}
    .add(15, event_hub::time_of_day(12, 0));
calendar.add_monthly_task(std::move(monthly), [] {});
```

`TaskManager` must outlive `CalendarScheduler`. Scheduler destruction cancels
queued one-shot tasks but does not wait for callbacks that already started.
Observers may be called from `cancel()`, `cancel_all()`, and destruction, so
callbacks and observers that capture external objects must guard those
objects' lifetimes.

## RunLoop

`RunLoop` is a convenience helper for applications that want one blocking loop
over several passive sources. It owns a `SyncNotifier`, registers it on added
`EventBus` and `TaskManager` instances, and processes them on the calling
thread until `request_stop()` is called.

```cpp
event_hub::EventBus bus;
event_hub::TaskManager tasks;
event_hub::RunLoop loop;

loop.add(bus);
loop.add(tasks);

tasks.post_after_ms(10, [&loop] {
    loop.request_stop();
});

loop.run();
```

Registered sources are non-owning and must outlive the loop. Add sources before
calling `run()`, and do not mutate the source list while the loop is running.

## Exception Handling

Without an exception handler, exceptions from event callbacks are rethrown from
`emit<T>()` or `process()`. With a handler, dispatch reports callback
exceptions and continues with the remaining callbacks and queued events:

```cpp
bus.set_exception_handler([](std::exception_ptr error) {
    try {
        if (error) {
            std::rethrow_exception(error);
        }
    } catch (const std::exception& ex) {
        // log or report ex.what()
    }
});
```

Exceptions from `AwaitOptions::on_timeout` never leave `poll_timeout() noexcept`.
They are reported to the bus exception handler when one is set; otherwise they
are ignored.

`TaskManager::set_exception_handler(...)` follows the same policy for task
callbacks. Without a handler, `TaskManager::process()` rethrows after restoring
unstarted work from the current batch. With a handler, it reports the exception
and continues with later tasks.

## Installation

### Install and `find_package`

After installing the library with CMake, consume it from another CMake project:

```bash
cmake -S . -B build -DEVENT_HUB_CPP_BUILD_TESTS=OFF -DEVENT_HUB_CPP_BUILD_EXAMPLES=OFF
cmake --install build --prefix /path/to/prefix
```

```cmake
cmake_minimum_required(VERSION 3.14)
project(app LANGUAGES CXX)

find_package(event-hub-cpp CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE event_hub::event_hub)
```

The exported target is `event_hub::event_hub`. The package also provides
`event-hub-cpp::event-hub-cpp` as a package-name alias.

### Git Submodule With `add_subdirectory`

Add the repository as a subdirectory and link the interface target:

```bash
git submodule add https://github.com/NewYaroslav/event-hub-cpp external/event-hub-cpp
```

```cmake
add_subdirectory(external/event-hub-cpp)
target_link_libraries(my_app PRIVATE event_hub::event_hub)
```

Then include the umbrella header:

```cpp
#include <event_hub.hpp>
```

### vcpkg Overlay

Install through the local overlay port:

```bash
vcpkg install event-hub-cpp --overlay-ports=./vcpkg-overlay/ports
```

Use the vcpkg toolchain when configuring your project:

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
```

Then use the same `find_package(event-hub-cpp CONFIG REQUIRED)` and
`target_link_libraries(... event_hub::event_hub)` calls.

### pkg-config

The install step also generates `event-hub-cpp.pc`:

```bash
c++ main.cpp -std=c++17 $(pkg-config --cflags --libs event-hub-cpp)
```

### Integration Notes

- `event_hub::event_hub` is a header-only target and has no external
  dependencies by default.
- Consumers need a C++17 or newer toolchain.
- When used via `add_subdirectory`, examples and tests are disabled by default
  unless `event-hub-cpp` is the top-level project.
- Optional `CalendarScheduler` utilities use `time-shield-cpp` from the
  `external/time-shield-cpp` submodule when configured with
  `-DEVENT_HUB_CPP_USE_TIME_SHIELD=ON`. NTP support is off by default; enable
  it with `-DEVENT_HUB_CPP_USE_TIME_SHIELD_NTP=ON`.

## Build And Test

```bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
ctest --test-dir build --output-on-failure
```

The top-level build creates:

- `event_hub_basic` from `examples/basic.cpp`;
- `event_hub_event_and_listener` from `examples/event_and_listener.cpp`;
- `event_hub_event_node` from `examples/event_node.cpp`;
- `event_hub_await_and_cancel` from `examples/await_and_cancel.cpp`;
- `event_hub_exception_handling` from `examples/exception_handling.cpp`;
- `event_hub_strict_lifetime` from `examples/strict_lifetime.cpp`;
- `event_hub_external_notifier` from `examples/external_notifier.cpp`;
- `event_hub_task_manager` from `examples/task_manager.cpp`;
- `event_hub_task_manager_periodic` from `examples/task_manager_periodic.cpp`;
- `event_hub_task_manager_with_bus` from `examples/task_manager_with_bus.cpp`;
- `event_hub_run_loop` from `examples/run_loop.cpp`;
- `event_hub_test_*` behavior tests from `tests/test_*.cpp`.

## Tested Platforms

| Platform | CI runner | C++ standards |
| --- | --- | --- |
| Windows | `windows-latest` | C++17, C++20 |
| Linux | `ubuntu-latest` | C++17, C++20 |
| macOS | `macos-latest` | C++17, C++20 |

CI also verifies installation consumption through `find_package` and validates
the vcpkg overlay port on Linux.

## Examples

- [basic.cpp](examples/basic.cpp) - plain value events, subscribe, emit,
  post/process, targeted unsubscribe, pending count, and queue clearing.
- [event_and_listener.cpp](examples/event_and_listener.cpp) - optional
  `event_hub::Event` inheritance, `EVENT_HUB_EVENT`, `EventListener`,
  `on_event()`, and `as_ref<T>()`.
- [event_node.cpp](examples/event_node.cpp) - `EventNode` as a module base
  with `listen`, protected `post`/`emit`, callback `subscribe`, and
  unsubscribe/close behavior.
- [await_and_cancel.cpp](examples/await_and_cancel.cpp) - `await_once()`,
  `await_each()`, manual awaiter cancellation, cancellation tokens, and timeout
  callbacks.
- [exception_handling.cpp](examples/exception_handling.cpp) -
  `set_exception_handler(...)`, handled callback failures, and fail-fast
  behavior without a handler.
- [strict_lifetime.cpp](examples/strict_lifetime.cpp) - advanced
  `std::shared_ptr` module lifetime with `weak_from_this()`.
- [external_notifier.cpp](examples/external_notifier.cpp) - one
  `SyncNotifier` shared by `EventBus` and `TaskManager`.
- [task_manager.cpp](examples/task_manager.cpp) - standalone `TaskManager`
  loop with priority, delayed tasks, and `submit()`.
- [task_manager_periodic.cpp](examples/task_manager_periodic.cpp) -
  periodic `TaskManager` work with immediate start, delayed first cycle,
  fixed-delay/fixed-rate scheduling, context self-cancel, self-reschedule, and
  handled failures.
- [calendar_scheduler.cpp](examples/calendar_scheduler.cpp) - optional
  `CalendarScheduler` rules over `TaskManager`; requires
  `EVENT_HUB_CPP_USE_TIME_SHIELD=ON` for the full example.
- [calendar_scheduler_ntp.cpp](examples/calendar_scheduler_ntp.cpp) -
  NTP-backed `CalendarScheduler` configuration through time-shield; requires
  `EVENT_HUB_CPP_USE_TIME_SHIELD=ON` and
  `EVENT_HUB_CPP_USE_TIME_SHIELD_NTP=ON`.
- [calendar_scheduler_custom_time.cpp](examples/calendar_scheduler_custom_time.cpp) -
  custom UTC millisecond time provider with a CET rule around the winter-time
  transition.
- [task_manager_with_bus.cpp](examples/task_manager_with_bus.cpp) - manual
  shared-notifier loop for `EventBus` and `TaskManager`.
- [run_loop.cpp](examples/run_loop.cpp) - convenience `RunLoop` over one bus
  and multiple task managers.

## Dispatch And Threading

`post<T>()` is safe to call from producer threads. `emit<T>()` and `process()`
invoke matching callbacks on the calling thread. The bus copies the callback
list before dispatch so handlers may subscribe or unsubscribe while handling an
event.

Subscription delivery policy controls which source can invoke a callback.
`subscribe<T>(DeliveryPolicy::direct, ...)` and `subscribe_direct<T>(...)`
accept only immediate `emit<T>()` delivery. `subscribe<T>(DeliveryPolicy::queued,
...)` and `subscribe_queued<T>(...)` accept only queued `process()` delivery.
`subscribe_any<T>(...)` keeps the compatibility `DeliveryPolicy::any` behavior.
Use queued subscriptions for module state that assumes hub-thread or run-loop
affinity; use direct subscriptions only for callbacks that are thread-safe and
reentrancy-safe.

`EventBus` and `TaskManager` do not own a thread and do not decide how long to
sleep. When a non-owning `INotifier` is set, each successful `post()` calls
`notify()` after work is queued. This lets an application event loop wait on one
shared notifier used by the bus, task manager, timers, or other sources:

```cpp
event_hub::SyncNotifier notifier;
event_hub::EventBus bus;
bus.set_notifier(&notifier);

event_hub::TaskManager tasks;
tasks.set_notifier(&notifier);

while (running) {
    // Capture the notifier state before processing so wake-ups that arrive
    // before the wait are still observed.
    const auto generation = notifier.generation();

    std::size_t work_done = 0;
    work_done += bus.process();
    // Bound task work per pass so other passive sources keep getting turns.
    work_done += tasks.process(128);

    if (work_done != 0) {
        continue;
    }

    // Sleep for at most 1 ms, or less if a delayed task is due sooner.
    const auto timeout =
        tasks.recommend_wait_for_ms(1);
    if (!bus.has_pending() && !tasks.has_ready()) {
        // If the generation changed after processing, this returns immediately.
        notifier.wait_for(generation, timeout);
    }
}
```

`SyncNotifier` uses a generation counter so a notification posted between
`generation()` and `wait_for(...)` is not lost; the wait returns immediately
when it observes that the generation has already changed.

Use `RunLoop` when that manual pattern should be packaged into a small
current-thread helper. It still uses the same notifier model and does not create
or own a worker thread.

`EventBus` must outlive every `EventEndpoint` and `EventAwaiter` that references
it.

`EventEndpoint` subscriptions use an internal lifetime guard. If a callback was
copied by `dispatch()` but the endpoint guard expires before callback start, the
callback is skipped. `unsubscribe_all()` does not wait for callbacks that have
already started or already passed the guard check.

For modules owned by `std::shared_ptr`, pass `weak_from_this()` as an explicit
guard when callbacks capture module state:

```cpp
class TokenModule : public std::enable_shared_from_this<TokenModule> {
public:
    void start() {
        auto weak = weak_from_this();

        m_endpoint.subscribe_queued<TokenFoundEvent>(
            weak,
            [weak](const TokenFoundEvent& event) {
                auto self = weak.lock();
                if (!self) {
                    return;
                }

                self->on_token_found(event);
            });
    }

private:
    void on_token_found(const TokenFoundEvent& event);

    event_hub::EventEndpoint m_endpoint;
};
```

Create such modules through `std::shared_ptr` before calling `start()`:

```cpp
auto module = std::make_shared<TokenModule>(bus);
module->start();
```

The endpoint guard prevents callbacks from starting after the endpoint is
closed or destroyed. The module `weak_from_this()` guard protects the module
itself:

```cpp
[weak](const TokenFoundEvent& event) {
    if (auto self = weak.lock()) {
        self->on_token_found(event);
    }
}
```

The basic model is intentionally simple:

- `post()` may be called from producer threads.
- Prefer calling `process()`, `emit()`, `subscribe()`, and `unsubscribe()` from
  the application/event-loop thread.
- `EventEndpoint::~EventEndpoint()` closes the endpoint, removes
  subscriptions, and prevents new guarded callbacks from starting.
- `EventEndpoint::~EventEndpoint()` does not wait for callbacks that already
  started.

See [examples/strict_lifetime.cpp](examples/strict_lifetime.cpp) for a complete
module example.

Events posted by handlers are processed by a later `process()` call, because
`process()` drains the queue snapshot captured at the start of the call.

Use `clear_pending()` to drop queued events without removing subscriptions.
`clear()` remains as a compatibility wrapper for the same operation.

## License

MIT. See [LICENSE](LICENSE).
