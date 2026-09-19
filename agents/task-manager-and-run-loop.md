# TaskManager And RunLoop Guidelines

This project includes a lightweight task layer beside the typed event bus. Keep
it passive, explicit, and dependency-light.

## Why TaskManager Exists

`EventBus` is for typed module communication. `TaskManager` is for ordinary
`void()` work that should run on the same application/event-loop thread without
turning the bus into a task scheduler.

The design intentionally mirrors `EventBus`:

- producers can submit work from other threads;
- user callbacks run only when the application calls `process()`;
- no library-owned worker thread, `run()`, `join()`, or blocking destructor;
- one shared `INotifier`/`SyncNotifier` can wake an external loop for both
  events and tasks.

This keeps the library usable in UI loops, game loops, service loops, tests, and
embedded-style polling code without forcing a thread ownership model.

## TaskManager Implementation Model

Core files:

- `include/event_hub/task.hpp` defines `TaskId`, `TaskPriority`,
  `TaskOptions`, `PeriodicSchedule`, `PeriodicTaskOptions`, and move-only
  `Task`.
- `include/event_hub/task_manager.hpp` defines the passive task queue.
- `include/event_hub/calendar_scheduler.hpp` defines the public
  `CalendarScheduler` class.
- `include/event_hub/calendar_scheduler/types.hpp` holds aggregate-owned public
  calendar DTOs, observer payloads, policies, and options. It is exposed
  through `calendar_scheduler.hpp`, not as a standalone include contract.
- `include/event_hub/calendar_scheduler/detail.hpp` holds aggregate-owned
  internal next-run calculation helpers and scheduler state. It is never a
  consumer include target.

Important choices:

- `Task` is a local move-only type erasure wrapper because C++17
  `std::function<void()>` cannot store move-only callables such as lambdas with
  `std::unique_ptr` captures or `std::packaged_task`.
- `Task` accepts both `void()` and `void(TaskContext&)` callbacks. Use
  `TaskContext` for self-cancel and explicit reschedule requests, not as an
  owning task object.
- Ready tasks use three FIFO `std::deque` queues: high, normal, low.
- Delayed and not-yet-ready periodic tasks use a min-heap ordered by
  `std::chrono::steady_clock` deadlines and a sequence number for stable
  ordering.
- Periodic tasks keep one `TaskId` for the full series. They may be scheduled
  as fixed-delay or fixed-rate work, and the first cycle is ready immediately
  unless the caller provides an initial delay.
- Cancellation is lazy: `TaskId` maps to an atomic task state, and containers
  are pruned or skipped later. Do not add eager O(n) removal as the main path.
- `TaskId` values are monotonic within one manager lifetime; `0` means invalid
  or rejected.
- `add_task_at_system(...)` converts a
  `std::chrono::system_clock::time_point` to a steady-clock deadline at
  submission time. It does not track later system clock changes and should not
  be described as a full calendar scheduler.
- `CalendarScheduler` is the calendar scheduler layer. It owns daily, weekly,
  monthly, or custom rules, computes UTC Unix-millisecond planned times with
  `time-shield-cpp`, then posts one steady-clock task into `TaskManager`.
  It still runs only when the application calls `TaskManager::process()`.
- `process()` takes a snapshot of ready work. Tasks posted by callbacks run on a
  later `process()` call, matching `EventBus`.
- User callbacks must never run while the queue mutex is held.
- Without an exception handler, `process()` restores unstarted work from the
  current batch and rethrows. With a handler, it reports and continues.
  Periodic tasks stop after a callback exception in both modes.

## TaskContext Rules

`TaskContext` is opt-in. Keep ordinary `void()` callbacks for simple work, and
use `void(event_hub::TaskContext&)` only when the callback needs the running
task id, self-cancel, or explicit rescheduling.

Supported scheduling APIs:

- `post`, `post_after`, `post_after_ms`, and `post_at` accept context-aware
  callables.
- `post_every`, `post_every_after`, `post_every_ms`, and
  `post_every_after_ms` accept context-aware callables.
- `post_batch(std::vector<Task>)` accepts context-aware tasks when they are
  constructed as `Task([](TaskContext& self) { ... })`.
- `submit()` intentionally stays no-context because it wraps a
  `std::packaged_task` and returns a future result.

Runtime semantics:

- A context is valid only while its callback is running. Do not store it as a
  long-lived handle; operations on a copied context after the callback returns
  must be expected to return `false`.
- `self.id()` is the stable `TaskId` for that one-shot task or the whole
  periodic series.
- `self.cancel()` does not interrupt the current callback. It cancels future
  periodic cycles or any reschedule requested by the current one-shot callback.
- If `self.cancel()` and `self.reschedule_at/after()` are both called in one
  callback, cancellation wins.
- One-shot `self.reschedule_at/after()` requeues the same callback with the same
  `TaskId`. Periodic explicit reschedule overrides only the next cycle; after
  that cycle, fixed-delay or fixed-rate scheduling continues normally.
- A due time in the past becomes ready for a later `process()` call, preserving
  snapshot semantics. It must not run again in the same snapshot batch.
- If the callback throws, any requested reschedule is ignored and the normal
  `TaskManager` exception policy applies.

## CalendarScheduler Rules

Use `CalendarScheduler` only for wall-clock calendar patterns such as "daily at
18:00", selected weekdays, selected month days, or a user-provided next-time
function. Keep `TaskManager` itself as a steady-clock executor.

Important semantics:

- `CalendarScheduler` is available only when
  `EVENT_HUB_CPP_USE_TIME_SHIELD=ON`; do not include
  `calendar_scheduler.hpp` in dependency-free examples unless guarded.
- Default timezone is UTC/GMT through `time_shield::ZonedClock{}`. A custom
  `now_provider` must return UTC Unix epoch milliseconds; the `ZonedClock`
  still defines how local daily/weekly/monthly rules are interpreted.
- Prefer `CalendarTaskOptions::in_zone(...)`,
  `CalendarTaskOptions::fixed_utc_offset(...)`, and
  `CalendarTaskOptions::with_clock(...)` in examples instead of hand-mutating
  fields. Use `with_clock(time_shield::ZonedClock(zone, true))` for
  time-shield NTP-backed UTC time in builds configured with
  `EVENT_HUB_CPP_USE_TIME_SHIELD_NTP=ON`.
- Prefer `time_of_day(hour, minute, second)` overloads and
  `WeeklySchedule::add(...)` / `MonthlySchedule::add(...)` builders in public
  examples. Keep `int second_of_day` overloads for low-level or generated
  schedules. `time_of_day(...)` means time inside the configured calendar day,
  not the host machine's current local time. Calendar rules use second
  granularity, so nonzero milliseconds are rejected by daily/weekly/monthly
  APIs.
- The scheduler computes delay from its observed UTC now and submits
  `TaskManager::add_task_at(...)`. This keeps NTP or custom time providers from
  being silently reinterpreted through `system_clock::now()`.
- For named zones, nonexistent local times in DST gaps are skipped. Ambiguous
  local times in DST folds are scheduled once at the first UTC occurrence.
- `CalendarTaskId` is stable for the calendar rule. The `TaskContext` seen by a
  callback is the current one-shot TaskManager task; calling `self.cancel()`
  stops future calendar scheduling after the current callback returns.
- The `TaskManager` passed to `CalendarScheduler` must outlive the scheduler.
  Scheduler destruction cancels queued one-shot tasks but does not wait for
  callbacks already running.
- Default missed-run policy is `skip`. Use `run_once_immediately` when a
  persisted `last_due_utc_ms` should produce one catch-up run.
- Default overlap/late-after-callback policy is `skip`. Use
  `queue_one_after_current` when one overdue occurrence should be queued for the
  next `process()` pass.
- Observer callbacks are for persistence and telemetry only. They should not be
  required for correctness, and scheduler code must not hold internal locks
  while invoking them. Observers may run from `cancel()`, `cancel_all()`, and
  destruction; user code owns the lifetime of captured objects.

## Correct Manual Loop

When manually combining buses and task managers, capture the notifier generation
before processing, then process every source, then wait on that old generation:

```cpp
while (running) {
    const auto generation = notifier.generation();

    std::size_t work_done = 0;
    work_done += bus.process();
    work_done += tasks.process(128);

    if (work_done != 0) {
        continue;
    }

    const auto timeout =
        tasks.recommend_wait_for(std::chrono::milliseconds(1));
    if (!bus.has_pending() && !tasks.has_ready()) {
        notifier.wait_for(generation, timeout);
    }
}
```

Do not read `generation()` after processing. That can lose a wakeup that arrives
between the processing phase and the actual wait.

## RunLoop Utility

`include/event_hub/run_loop.hpp` provides `RunLoop`, a small blocking helper for
applications that want a ready-made loop.

Use it when an app wants one current-thread loop over many passive sources:

```cpp
event_hub::EventBus bus;
event_hub::TaskManager tasks;
event_hub::RunLoop loop;

loop.add(bus);
loop.add(tasks);
loop.run();
```

RunLoop owns a `SyncNotifier`, registers it on every added `EventBus` and
`TaskManager`, and blocks the calling thread until `request_stop()` is called.
It does not create a thread. Registered sources are non-owning pointers; add
them before `run()`, keep them alive longer than the loop, and do not mutate the
source list concurrently with processing.

Prefer manual loops in examples that teach the underlying model. Use `RunLoop`
examples only when demonstrating the convenience helper itself.

## Usage Rules

- Attach the same notifier to all passive sources that share one loop.
- Call `reset_notifier()` before destroying a notifier manually. `RunLoop`
  handles this for sources it registered, assuming those sources still exist.
- Use `submit()` when a task result is needed through `std::future`.
- Use `post_batch()` when a producer has several immediate tasks and should
  wake the loop once.
- Use `add_task_at(...)` or `post_at(...)` when the caller already has a
  `TaskManager::TimePoint` / `std::chrono::steady_clock::time_point` deadline.
  Treat this as a monotonic technical deadline, not a wall-clock calendar time.
- Use `add_task_at_system(...)` for simple one-shot wall-clock submissions.
  Use `add_task_at_system_ms(...)` when the wall-clock deadline is stored as
  Unix epoch milliseconds. Calendar patterns such as "daily at 18:00" belong in
  a separate `CalendarScheduler` layer over `TaskManager`.
- Use `CalendarScheduler::add_daily_task`, `add_weekly_task`,
  `add_monthly_task`, or `add_custom_calendar_task` for calendar rules when the
  optional time-shield integration is enabled.
- Use `post_every(...)` / `post_every_after(...)` for lightweight repeating
  work. Prefer fixed-delay unless the caller needs planned-deadline timing.
- Prefer `TaskContext&` callbacks when a task needs to cancel or reschedule
  itself; avoid awkward `TaskId` capture patterns in examples.
- Keep task callbacks short; publish events or enqueue follow-up tasks for
  larger workflows.
- Treat `process()`, `clear_pending()`, `close()`, and destruction as
  single-consumer/lifetime operations.

## Testing Expectations

Task-related changes should cover:

- immediate and delayed execution;
- periodic fixed-delay and fixed-rate execution;
- context-aware callbacks, including self-cancel and one-shot reschedule;
- priority ordering and FIFO within one priority;
- snapshot semantics for tasks posted by callbacks;
- cancellation before start, between periodic cycles, from inside a periodic
  callback, and rejection after one-shot start;
- exception handler and fail-fast restore behavior;
- notifier wakeups and `recommend_wait_for(...)`;
- `submit()` futures and move-only task captures;
- `RunLoop` with multiple task managers and at least one `EventBus`.
- `CalendarScheduler` daily/weekly/monthly next-time calculations, missed-run
  and overlap policies, observer event order, custom providers, and
  cancellation when time-shield integration is enabled.
