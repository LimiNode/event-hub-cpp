#pragma once
#ifndef EVENT_HUB_TASK_MANAGER_HPP_INCLUDED
#define EVENT_HUB_TASK_MANAGER_HPP_INCLUDED

/// \file task_manager.hpp
/// \brief Defines a passive task manager for external event loops.

#include "notifier.hpp"
#include "task.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace event_hub {

/// \class TaskManager
/// \brief Passive multi-producer, single-consumer task manager.
///
/// TaskManager does not own threads and never waits on its own. Producer
/// threads submit tasks, while an application-owned event loop calls process().
/// A non-owning INotifier can be shared with EventBus to wake that loop.
///
/// Threading:
/// - post variants, submit, cancel, count queries, and notifier setters are
///   thread-safe.
/// - process(), clear_pending(), close(), and destruction are single-consumer
///   lifetime APIs and must not be called concurrently with each other.
class TaskManager {
public:
    using Clock = std::chrono::steady_clock; ///< Monotonic clock used for delayed tasks.
    using SystemClock = std::chrono::system_clock; ///< Wall-clock type accepted by add_task_at_system().
    using TimePoint = Clock::time_point; ///< Time point type used by delayed tasks.
    using SystemTimePoint = SystemClock::time_point; ///< Wall-clock time point converted on submit.
    using Duration = Clock::duration; ///< Duration type used by delayed tasks.
    using ExceptionHandler = std::function<void(std::exception_ptr)>; ///< Callback for task callback exceptions.

    /// \brief Construct an empty manager.
    TaskManager() = default;

    /// \brief Construct an empty manager with a non-owning notifier.
    explicit TaskManager(INotifier* notifier) noexcept
        : m_notifier(notifier) {}

    /// \brief Close and drop pending tasks without waiting for callbacks.
    ~TaskManager() {
        close();
        clear_pending();
    }

    TaskManager(const TaskManager&) = delete;
    TaskManager& operator=(const TaskManager&) = delete;
    TaskManager(TaskManager&&) = delete;
    TaskManager& operator=(TaskManager&&) = delete;

    /// \brief Set a non-owning notifier called after accepted submissions.
    ///
    /// The caller must keep the notifier alive while producers may submit work.
    /// Producers must be quiescent before reset_notifier() or notifier
    /// destruction; reset_notifier() does not wait for an in-flight producer.
    void set_notifier(INotifier* notifier) noexcept {
        m_notifier.store(notifier, std::memory_order_release);
    }

    /// \brief Remove the currently configured notifier.
    void reset_notifier() noexcept {
        m_notifier.store(nullptr, std::memory_order_release);
    }

    /// \brief Set a handler for exceptions thrown by task callbacks.
    ///
    /// When a handler is set, process() reports callback exceptions and
    /// continues. Without a handler, process() restores unstarted work from the
    /// current batch and rethrows the callback exception.
    void set_exception_handler(ExceptionHandler handler) {
        std::lock_guard<std::mutex> lock(m_exception_handler_mutex);
        m_exception_handler = handler
                                  ? std::make_shared<ExceptionHandler>(
                                        std::move(handler))
                                  : nullptr;
    }

    /// \brief Stop accepting new tasks.
    ///
    /// This call is non-blocking and does not wait for already started
    /// callbacks. Queued tasks may still be processed or dropped with
    /// clear_pending().
    void close() noexcept {
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            notify = !m_closed.exchange(true, std::memory_order_acq_rel);
        }

        if (notify) {
            notify_work_available();
        }
    }

    /// \brief Return true when the manager no longer accepts new tasks.
    bool is_closed() const noexcept {
        return m_closed.load(std::memory_order_acquire);
    }

    /// \brief Submit an immediate task.
    ///
    /// \return The task id, or zero when the task is empty or the manager is
    /// closed.
    TaskId post(Task task, TaskOptions options = {}) {
        if (!task) {
            return 0;
        }

        const auto id = enqueue_ready(std::move(task), options);
        if (id != 0) {
            notify_work_available();
        }
        return id;
    }

    /// \brief Submit an immediate callable.
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    TaskId post(F&& fn, TaskOptions options = {}) {
        return post(Task(std::forward<F>(fn)), options);
    }

    /// \brief Submit a task to run after a delay.
    template <typename Rep, typename Period>
    TaskId post_after(const std::chrono::duration<Rep, Period>& delay,
                      Task task,
                      TaskOptions options = {}) {
        const auto due =
            Clock::now() + std::chrono::duration_cast<Duration>(delay);
        return post_at(due, std::move(task), options);
    }

    /// \brief Submit a callable to run after a delay.
    template <
        typename Rep,
        typename Period,
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    TaskId post_after(const std::chrono::duration<Rep, Period>& delay,
                      F&& fn,
                      TaskOptions options = {}) {
        const auto due =
            Clock::now() + std::chrono::duration_cast<Duration>(delay);
        return post_at(due, Task(std::forward<F>(fn)), options);
    }

    /// \brief Submit a task to run after delay_ms milliseconds.
    TaskId post_after_ms(std::int64_t delay_ms,
                         Task task,
                         TaskOptions options = {}) {
        return post_after(std::chrono::milliseconds(delay_ms),
                          std::move(task),
                          options);
    }

    /// \brief Submit a callable to run after delay_ms milliseconds.
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    TaskId post_after_ms(std::int64_t delay_ms,
                         F&& fn,
                         TaskOptions options = {}) {
        return post_after(std::chrono::milliseconds(delay_ms),
                          Task(std::forward<F>(fn)),
                          options);
    }

    /// \brief Submit a periodic task whose first cycle is ready immediately.
    ///
    /// The task runs only from process(). The returned id identifies the whole
    /// periodic series and can be passed to cancel().
    ///
    /// \return The task id, or zero when interval is non-positive, the task is
    /// empty, or the manager is closed.
    template <typename Rep, typename Period>
    TaskId post_every(const std::chrono::duration<Rep, Period>& interval,
                      Task task,
                      PeriodicTaskOptions options = {}) {
        return post_every_after(Duration::zero(),
                                interval,
                                std::move(task),
                                options);
    }

    /// \brief Submit a periodic callable whose first cycle is ready immediately.
    template <
        typename Rep,
        typename Period,
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    TaskId post_every(const std::chrono::duration<Rep, Period>& interval,
                      F&& fn,
                      PeriodicTaskOptions options = {}) {
        return post_every(interval,
                          Task(std::forward<F>(fn)),
                          options);
    }

    /// \brief Submit a periodic task with an initial delay before the first run.
    ///
    /// A non-positive initial delay makes the first cycle ready immediately.
    /// A non-positive interval rejects the submission.
    template <typename DelayRep,
              typename DelayPeriod,
              typename IntervalRep,
              typename IntervalPeriod>
    TaskId post_every_after(
        const std::chrono::duration<DelayRep, DelayPeriod>& initial_delay,
        const std::chrono::duration<IntervalRep, IntervalPeriod>& interval,
        Task task,
        PeriodicTaskOptions options = {}) {
        if (!task) {
            return 0;
        }

        const auto period = std::chrono::duration_cast<Duration>(interval);
        if (period <= Duration::zero()) {
            return 0;
        }

        const auto now = Clock::now();
        const auto delay = std::chrono::duration_cast<Duration>(initial_delay);
        const auto due = delay <= Duration::zero() ? now : now + delay;

        TaskId id = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed.load(std::memory_order_acquire)) {
                return 0;
            }

            id = enqueue_periodic_locked(due,
                                         period,
                                         std::move(task),
                                         options);
        }

        if (id != 0) {
            notify_work_available();
        }
        return id;
    }

    /// \brief Submit a periodic callable with an initial delay.
    template <
        typename DelayRep,
        typename DelayPeriod,
        typename IntervalRep,
        typename IntervalPeriod,
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    TaskId post_every_after(
        const std::chrono::duration<DelayRep, DelayPeriod>& initial_delay,
        const std::chrono::duration<IntervalRep, IntervalPeriod>& interval,
        F&& fn,
        PeriodicTaskOptions options = {}) {
        return post_every_after(initial_delay,
                                interval,
                                Task(std::forward<F>(fn)),
                                options);
    }

    /// \brief Submit a periodic task with an interval in milliseconds.
    TaskId post_every_ms(std::int64_t interval_ms,
                         Task task,
                         PeriodicTaskOptions options = {}) {
        return post_every(std::chrono::milliseconds(interval_ms),
                          std::move(task),
                          options);
    }

    /// \brief Submit a periodic callable with an interval in milliseconds.
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    TaskId post_every_ms(std::int64_t interval_ms,
                         F&& fn,
                         PeriodicTaskOptions options = {}) {
        return post_every(std::chrono::milliseconds(interval_ms),
                          Task(std::forward<F>(fn)),
                          options);
    }

    /// \brief Submit a periodic task with millisecond initial delay/interval.
    TaskId post_every_after_ms(std::int64_t initial_delay_ms,
                               std::int64_t interval_ms,
                               Task task,
                               PeriodicTaskOptions options = {}) {
        return post_every_after(std::chrono::milliseconds(initial_delay_ms),
                                std::chrono::milliseconds(interval_ms),
                                std::move(task),
                                options);
    }

    /// \brief Submit a periodic callable with millisecond delay/interval.
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    TaskId post_every_after_ms(std::int64_t initial_delay_ms,
                               std::int64_t interval_ms,
                               F&& fn,
                               PeriodicTaskOptions options = {}) {
        return post_every_after(std::chrono::milliseconds(initial_delay_ms),
                                std::chrono::milliseconds(interval_ms),
                                Task(std::forward<F>(fn)),
                                options);
    }

    /// \brief Submit a task to run at a steady-clock time point.
    ///
    /// \return The task id, or zero when the task is empty or the manager is
    /// closed.
    TaskId post_at(TimePoint due, Task task, TaskOptions options = {}) {
        if (!task) {
            return 0;
        }

        TaskId id = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed.load(std::memory_order_acquire)) {
                return 0;
            }

            if (due <= Clock::now()) {
                id = enqueue_ready_locked(std::move(task), options);
            } else {
                id = enqueue_delayed_locked(due, std::move(task), options);
            }
        }

        if (id != 0) {
            notify_work_available();
        }
        return id;
    }

    /// \brief Submit a callable to run at a steady-clock time point.
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    TaskId post_at(TimePoint due, F&& fn, TaskOptions options = {}) {
        return post_at(due, Task(std::forward<F>(fn)), options);
    }

    /// \brief Submit a task to run at a steady-clock deadline.
    ///
    /// Alias for post_at() using explicit task/deadline naming. The deadline is
    /// monotonic, not a wall-clock calendar time.
    ///
    /// \return The task id, or zero when the task is empty or the manager is
    /// closed.
    TaskId add_task_at(TimePoint deadline,
                       Task task,
                       TaskOptions options = {}) {
        return post_at(deadline, std::move(task), options);
    }

    /// \brief Submit a callable to run at a steady-clock deadline.
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    TaskId add_task_at(TimePoint deadline,
                       F&& fn,
                       TaskOptions options = {}) {
        return add_task_at(deadline, Task(std::forward<F>(fn)), options);
    }

    /// \brief Submit a task to run at a system-clock time point.
    ///
    /// The system-clock time is converted to a steady-clock deadline when this
    /// function is called. Later system clock changes do not move the task.
    ///
    /// \return The task id, or zero when the task is empty or the manager is
    /// closed.
    TaskId add_task_at_system(SystemTimePoint time,
                              Task task,
                              TaskOptions options = {}) {
        const auto delay =
            std::chrono::duration_cast<Duration>(time - SystemClock::now());
        const auto deadline = Clock::now() + delay;
        return post_at(deadline, std::move(task), options);
    }

    /// \brief Submit a callable to run at a system-clock time point.
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    TaskId add_task_at_system(SystemTimePoint time,
                              F&& fn,
                              TaskOptions options = {}) {
        return add_task_at_system(time,
                                  Task(std::forward<F>(fn)),
                                  options);
    }

    /// \brief Submit a task at a Unix epoch time in milliseconds.
    ///
    /// The millisecond timestamp is interpreted as system-clock time and is
    /// converted to a steady-clock deadline when this function is called.
    TaskId add_task_at_system_ms(std::int64_t unix_time_ms,
                                 Task task,
                                 TaskOptions options = {}) {
        const auto duration = std::chrono::duration_cast<
            SystemClock::duration>(std::chrono::milliseconds(unix_time_ms));
        return add_task_at_system(SystemTimePoint(duration),
                                  std::move(task),
                                  options);
    }

    /// \brief Submit a callable at a Unix epoch time in milliseconds.
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    TaskId add_task_at_system_ms(std::int64_t unix_time_ms,
                                 F&& fn,
                                 TaskOptions options = {}) {
        return add_task_at_system_ms(unix_time_ms,
                                     Task(std::forward<F>(fn)),
                                     options);
    }

    /// \brief Submit a batch of immediate tasks with one notification.
    ///
    /// Returned ids preserve input order. Zero means the corresponding task was
    /// empty or the manager was already closed.
    std::vector<TaskId> post_batch(std::vector<Task> tasks,
                                   TaskOptions options = {}) {
        std::vector<TaskId> ids(tasks.size(), 0);
        bool notify = false;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed.load(std::memory_order_acquire)) {
                return ids;
            }

            for (std::size_t i = 0; i != tasks.size(); ++i) {
                if (!tasks[i]) {
                    continue;
                }

                ids[i] = enqueue_ready_locked(std::move(tasks[i]), options);
                notify = true;
            }
        }

        if (notify) {
            notify_work_available();
        }
        return ids;
    }

    /// \brief Submit a callable and receive its future result.
    ///
    /// \throws std::runtime_error when the manager rejects the task.
    template <
        typename F,
        typename Fn = typename std::decay<F>::type,
        typename std::enable_if<std::is_invocable<Fn&>::value, int>::type = 0>
    auto submit(F&& fn, TaskOptions options = {})
        -> std::future<typename std::invoke_result<Fn&>::type> {
        using Result = typename std::invoke_result<Fn&>::type;
        std::packaged_task<Result()> packaged(std::forward<F>(fn));
        auto future = packaged.get_future();

        const auto id = post(
            [task = std::move(packaged)]() mutable {
                task();
            },
            options);

        if (id == 0) {
            throw std::runtime_error("TaskManager rejected task");
        }

        return future;
    }

    /// \brief Cancel a task that has not started.
    ///
    /// \return True when the task was queued or in the current process()
    /// snapshot and is now cancelled. For an executing periodic task, returns
    /// true when future cycles are cancelled.
    bool cancel(TaskId id) noexcept {
        if (id == 0) {
            return false;
        }

        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_index.find(id);
            if (it == m_index.end()) {
                return false;
            }

            auto& control = it->second;
            auto state = control->state.load(std::memory_order_acquire);
            switch (state) {
            case State::queued_ready:
                if (!transition(*control, state, State::cancelled)) {
                    return false;
                }
                decrement_ready_count();
                decrement_pending_count();
                m_index.erase(it);
                cancelled = true;
                break;

            case State::queued_delayed:
                if (!transition(*control, state, State::cancelled)) {
                    return false;
                }
                decrement_pending_count();
                m_index.erase(it);
                cancelled = true;
                break;

            case State::ready_snapshot:
                if (!transition(*control, state, State::cancelled)) {
                    return false;
                }
                decrement_pending_count();
                m_index.erase(it);
                cancelled = true;
                break;

            case State::executing:
                if (!control->periodic) {
                    return false;
                }
                if (!transition(*control, state, State::cancelled)) {
                    return false;
                }
                decrement_pending_count();
                control->reschedule_requested = false;
                cancelled = true;
                break;

            case State::cancelled:
            case State::completed:
                return false;
            }
        }

        if (cancelled) {
            // Cancellation may move the earliest delayed deadline. Wake an
            // external loop so it can recompute its wait duration promptly.
            notify_work_available();
        }
        return cancelled;
    }

    /// \brief Execute up to max_tasks ready tasks.
    ///
    /// Delayed tasks due at process() entry are promoted first. Tasks submitted
    /// while callbacks run are processed by a later process() call.
    ///
    /// \return Number of callbacks that actually started.
    /// \throws Any task callback exception when no exception handler is set.
    std::size_t process(
        std::size_t max_tasks = (std::numeric_limits<std::size_t>::max)()) {
        if (max_tasks == 0) {
            return 0;
        }

        std::vector<Entry> batch;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            promote_due_locked(Clock::now());
            batch = take_ready_batch_locked(max_tasks);
        }

        std::size_t processed = 0;
        for (std::size_t i = 0; i != batch.size(); ++i) {
            auto& entry = batch[i];
            auto expected = State::ready_snapshot;
            if (!entry.control->state.compare_exchange_strong(
                    expected,
                    State::executing,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                continue;
            }

            if (!entry.periodic) {
                decrement_pending_count();
            }

            auto context = make_context(entry.id);
            // Keep the control block available if finishing the task throws
            // after Entry has been moved into a requeue operation.
            auto control = entry.control;
            try {
                entry.task(context);
                finish_after_success(std::move(entry));
                ++processed;
            } catch (...) {
                finish_after_exception(entry, std::move(control));
                const auto exception = std::current_exception();

                try {
                    if (report_exception(exception)) {
                        ++processed;
                        continue;
                    }
                } catch (...) {
                    restore_unrun(batch, i + 1);
                    throw;
                }

                restore_unrun(batch, i + 1);
                std::rethrow_exception(exception);
            }
        }

        return processed;
    }

    /// \brief Drop all queued tasks that have not started.
    ///
    /// \return Number of tasks cancelled by this call.
    std::size_t clear_pending() noexcept {
        std::size_t dropped = 0;
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& queue : m_ready) {
            for (auto& entry : queue) {
                auto state = State::queued_ready;
                if (entry.control->state.compare_exchange_strong(
                        state,
                        State::cancelled,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    decrement_ready_count();
                    decrement_pending_count();
                    m_index.erase(entry.id);
                    ++dropped;
                }
            }
            queue.clear();
        }

        for (auto& entry : m_delayed_heap) {
            auto state = State::queued_delayed;
            if (entry.control->state.compare_exchange_strong(
                    state,
                    State::cancelled,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                decrement_pending_count();
                m_index.erase(entry.id);
                ++dropped;
            }
        }
        m_delayed_heap.clear();

        return dropped;
    }

    /// \brief Return the current count of queued ready tasks.
    std::size_t ready_count() const noexcept {
        return m_ready_count.load(std::memory_order_relaxed);
    }

    /// \brief Return the current count of not-yet-started tasks.
    std::size_t pending_count() const noexcept {
        return m_pending_count.load(std::memory_order_relaxed);
    }

    /// \brief Return true when queued ready work exists.
    bool has_ready() const noexcept {
        return ready_count() != 0U;
    }

    /// \brief Return true when queued or snapshotted unstarted work exists.
    bool has_pending() const noexcept {
        return pending_count() != 0U;
    }

    /// \brief Return the earliest active delayed deadline, if one exists.
    std::optional<TimePoint> next_deadline() {
        std::lock_guard<std::mutex> lock(m_mutex);
        prune_delayed_top_locked();
        if (m_delayed_heap.empty()) {
            return std::nullopt;
        }

        return m_delayed_heap.front().due;
    }

    /// \brief Compute a recommended external wait duration.
    ///
    /// Returns zero when ready work exists, otherwise the smaller of idle_cap
    /// and the time remaining until the earliest delayed task.
    template <typename Rep, typename Period>
    Duration recommend_wait_for(
        const std::chrono::duration<Rep, Period>& idle_cap) {
        const auto cap = std::chrono::duration_cast<Duration>(idle_cap);
        if (has_ready()) {
            return Duration::zero();
        }

        const auto deadline = next_deadline();
        if (!deadline) {
            return cap;
        }

        const auto now = Clock::now();
        if (*deadline <= now) {
            return Duration::zero();
        }

        const auto until_deadline = *deadline - now;
        return until_deadline < cap ? until_deadline : cap;
    }

    /// \brief Compute a recommended external wait duration in milliseconds.
    Duration recommend_wait_for_ms(std::int64_t idle_cap_ms) {
        return recommend_wait_for(std::chrono::milliseconds(idle_cap_ms));
    }

private:
    enum class State : std::uint8_t {
        queued_ready,
        queued_delayed,
        ready_snapshot,
        executing,
        cancelled,
        completed
    };

    struct Control {
        Control(TaskId id_, State state_, bool periodic_) noexcept
            : id(id_),
              periodic(periodic_),
              state(state_) {}

        TaskId id = 0;
        bool periodic = false;
        bool reschedule_requested = false;
        TimePoint reschedule_due{};
        std::atomic<State> state{State::completed};
    };

    struct Entry {
        TaskId id = 0;
        std::uint64_t seq = 0;
        TaskPriority priority{TaskPriority::normal};
        TimePoint due{};
        bool periodic = false;
        Duration interval{Duration::zero()};
        PeriodicSchedule periodic_schedule{PeriodicSchedule::fixed_delay};
        TimePoint planned_due{};
        std::shared_ptr<Control> control;
        Task task;
    };

    struct DelayedSooner {
        bool operator()(const Entry& lhs, const Entry& rhs) const noexcept {
            if (lhs.due != rhs.due) {
                return lhs.due > rhs.due;
            }

            return lhs.seq > rhs.seq;
        }
    };

    static bool transition(Control& control,
                           State& expected,
                           State desired) noexcept {
        return control.state.compare_exchange_strong(
            expected,
            desired,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    static std::size_t priority_index(TaskPriority priority) noexcept {
        switch (priority) {
        case TaskPriority::high:
            return 0;
        case TaskPriority::normal:
            return 1;
        case TaskPriority::low:
            return 2;
        }

        return 1;
    }

    TaskId next_id() noexcept {
        auto id = m_next_id.fetch_add(1, std::memory_order_relaxed);
        if (id == 0) {
            id = m_next_id.fetch_add(1, std::memory_order_relaxed);
        }

        return id;
    }

    std::uint64_t next_seq() noexcept {
        return m_next_seq.fetch_add(1, std::memory_order_relaxed);
    }

    TaskId enqueue_ready(Task task, TaskOptions options) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed.load(std::memory_order_acquire)) {
            return 0;
        }

        return enqueue_ready_locked(std::move(task), options);
    }

    TaskId enqueue_ready_locked(Task task, TaskOptions options) {
        const auto id = next_id();
        auto control =
            std::make_shared<Control>(id, State::queued_ready, false);

        Entry entry;
        entry.id = id;
        entry.seq = next_seq();
        entry.priority = options.priority;
        entry.control = control;
        entry.task = std::move(task);

        m_index.emplace(id, std::move(control));
        m_ready[priority_index(options.priority)].push_back(std::move(entry));
        increment_ready_count();
        increment_pending_count();
        return id;
    }

    TaskId enqueue_delayed_locked(TimePoint due,
                                  Task task,
                                  TaskOptions options) {
        const auto id = next_id();
        auto control =
            std::make_shared<Control>(id, State::queued_delayed, false);

        Entry entry;
        entry.id = id;
        entry.seq = next_seq();
        entry.priority = options.priority;
        entry.due = due;
        entry.control = control;
        entry.task = std::move(task);

        m_index.emplace(id, control);
        m_delayed_heap.push_back(std::move(entry));
        std::push_heap(m_delayed_heap.begin(),
                       m_delayed_heap.end(),
                       DelayedSooner{});
        increment_pending_count();
        return id;
    }

    TaskId enqueue_periodic_locked(TimePoint due,
                                   Duration interval,
                                   Task task,
                                   PeriodicTaskOptions options) {
        const auto ready = due <= Clock::now();
        const auto state =
            ready ? State::queued_ready : State::queued_delayed;
        const auto id = next_id();
        auto control = std::make_shared<Control>(id, state, true);

        Entry entry;
        entry.id = id;
        entry.seq = next_seq();
        entry.priority = options.priority;
        entry.due = due;
        entry.periodic = true;
        entry.interval = interval;
        entry.periodic_schedule = options.schedule;
        entry.planned_due = due;
        entry.control = control;
        entry.task = std::move(task);

        m_index.emplace(id, control);
        if (ready) {
            m_ready[priority_index(options.priority)].push_back(
                std::move(entry));
            increment_ready_count();
        } else {
            m_delayed_heap.push_back(std::move(entry));
            std::push_heap(m_delayed_heap.begin(),
                           m_delayed_heap.end(),
                           DelayedSooner{});
        }

        increment_pending_count();
        return id;
    }

    void notify_work_available() noexcept {
        if (auto* notifier = m_notifier.load(std::memory_order_acquire)) {
            notifier->notify();
        }
    }

    void increment_ready_count() noexcept {
        m_ready_count.fetch_add(1, std::memory_order_relaxed);
    }

    void decrement_ready_count() noexcept {
        m_ready_count.fetch_sub(1, std::memory_order_relaxed);
    }

    void increment_pending_count() noexcept {
        m_pending_count.fetch_add(1, std::memory_order_relaxed);
    }

    void decrement_pending_count() noexcept {
        m_pending_count.fetch_sub(1, std::memory_order_relaxed);
    }

    std::shared_ptr<ExceptionHandler> exception_handler() const {
        std::lock_guard<std::mutex> lock(m_exception_handler_mutex);
        return m_exception_handler;
    }

    bool report_exception(std::exception_ptr exception) const {
        auto handler = exception_handler();
        if (!handler) {
            return false;
        }

        (*handler)(std::move(exception));
        return true;
    }

    TaskContext make_context(TaskId id) noexcept {
        return TaskContext(this,
                           id,
                           &TaskManager::context_cancel,
                           &TaskManager::context_is_cancelled,
                           &TaskManager::context_reschedule);
    }

    static bool context_cancel(void* owner, TaskId id) noexcept {
        return static_cast<TaskManager*>(owner)->cancel_from_context(id);
    }

    static bool context_is_cancelled(void* owner, TaskId id) noexcept {
        return static_cast<TaskManager*>(owner)->is_cancelled_for_context(id);
    }

    static bool context_reschedule(void* owner,
                                   TaskId id,
                                   TaskContext::TimePoint due) noexcept {
        return static_cast<TaskManager*>(owner)->reschedule_from_context(id,
                                                                        due);
    }

    bool cancel_from_context(TaskId id) noexcept {
        if (id == 0) {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_index.find(id);
        if (it == m_index.end()) {
            return false;
        }

        auto& control = it->second;
        auto state = control->state.load(std::memory_order_acquire);
        if (state != State::executing) {
            return false;
        }

        if (!transition(*control, state, State::cancelled)) {
            return false;
        }

        control->reschedule_requested = false;
        if (control->periodic) {
            decrement_pending_count();
        }
        return true;
    }

    bool is_cancelled_for_context(TaskId id) const noexcept {
        if (id == 0) {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_index.find(id);
        if (it == m_index.end()) {
            return false;
        }

        return it->second->state.load(std::memory_order_acquire) ==
               State::cancelled;
    }

    bool reschedule_from_context(TaskId id, TimePoint due) noexcept {
        if (id == 0) {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed.load(std::memory_order_acquire)) {
            return false;
        }

        auto it = m_index.find(id);
        if (it == m_index.end()) {
            return false;
        }

        auto& control = it->second;
        if (control->state.load(std::memory_order_acquire) !=
            State::executing) {
            return false;
        }

        control->reschedule_requested = true;
        control->reschedule_due = due;
        return true;
    }

    void prune_delayed_top_locked() {
        while (!m_delayed_heap.empty()) {
            const auto state =
                m_delayed_heap.front().control->state.load(
                    std::memory_order_acquire);
            if (state != State::cancelled && state != State::completed) {
                return;
            }

            std::pop_heap(m_delayed_heap.begin(),
                          m_delayed_heap.end(),
                          DelayedSooner{});
            m_delayed_heap.pop_back();
        }
    }

    void promote_due_locked(TimePoint now) {
        prune_delayed_top_locked();
        while (!m_delayed_heap.empty() && m_delayed_heap.front().due <= now) {
            std::pop_heap(m_delayed_heap.begin(),
                          m_delayed_heap.end(),
                          DelayedSooner{});
            auto entry = std::move(m_delayed_heap.back());
            m_delayed_heap.pop_back();

            auto state = State::queued_delayed;
            if (entry.control->state.compare_exchange_strong(
                    state,
                    State::queued_ready,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                m_ready[priority_index(entry.priority)].push_back(
                    std::move(entry));
                increment_ready_count();
            }

            prune_delayed_top_locked();
        }
    }

    std::vector<Entry> take_ready_batch_locked(std::size_t max_tasks) {
        std::vector<Entry> batch;
        batch.reserve(std::min(max_tasks, ready_count()));

        for (auto& queue : m_ready) {
            while (!queue.empty() && batch.size() != max_tasks) {
                auto entry = std::move(queue.front());
                queue.pop_front();

                auto state = State::queued_ready;
                if (!entry.control->state.compare_exchange_strong(
                        state,
                        State::ready_snapshot,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    continue;
                }

                decrement_ready_count();
                batch.push_back(std::move(entry));
            }
        }

        return batch;
    }

    TimePoint next_periodic_due(const Entry& entry, TimePoint now) const {
        if (entry.periodic_schedule == PeriodicSchedule::fixed_rate) {
            return entry.planned_due + entry.interval;
        }

        return now + entry.interval;
    }

    void enqueue_existing_locked(Entry entry, TimePoint due) {
        const auto now = Clock::now();
        const auto ready = due <= now;
        const auto desired =
            ready ? State::queued_ready : State::queued_delayed;

        entry.seq = next_seq();
        entry.due = due;
        entry.planned_due = due;
        const auto queue_index = priority_index(entry.priority);
        const bool periodic = entry.periodic;

        if (ready) {
            auto& queue = m_ready[queue_index];
            queue.push_back(std::move(entry));
            // Publish the state only after the potentially throwing
            // insertion succeeds; this prevents phantom controls when an
            // allocation fails.
            queue.back().control->state.store(desired,
                                              std::memory_order_release);
            if (!periodic) {
                increment_pending_count();
            }
            increment_ready_count();
            return;
        }

        m_delayed_heap.push_back(std::move(entry));
        m_delayed_heap.back().control->state.store(
            desired, std::memory_order_release);
        std::push_heap(m_delayed_heap.begin(),
                       m_delayed_heap.end(),
                       DelayedSooner{});
        if (!periodic) {
            increment_pending_count();
        }
    }

    void finish_after_success(Entry entry) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto state = entry.control->state.load(std::memory_order_acquire);
        if (state == State::cancelled) {
            entry.control->reschedule_requested = false;
            m_index.erase(entry.id);
            return;
        }

        if (state != State::executing) {
            return;
        }

        if (m_closed.load(std::memory_order_acquire)) {
            if (entry.control->state.compare_exchange_strong(
                    state,
                    State::completed,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                if (entry.periodic) {
                    decrement_pending_count();
                }
                entry.control->reschedule_requested = false;
                m_index.erase(entry.id);
            }
            return;
        }

        if (entry.control->reschedule_requested) {
            const auto due = entry.control->reschedule_due;
            entry.control->reschedule_requested = false;
            enqueue_existing_locked(std::move(entry), due);
            return;
        }

        if (entry.periodic) {
            const auto due = next_periodic_due(entry, Clock::now());
            enqueue_existing_locked(std::move(entry), due);
            return;
        }

        if (entry.control->state.compare_exchange_strong(
                state,
                State::completed,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            m_index.erase(entry.id);
        }
    }

    void finish_after_exception(
        Entry& entry, const std::shared_ptr<Control>& control) {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!control) {
            return;
        }

        control->reschedule_requested = false;

        auto state = control->state.load(std::memory_order_acquire);
        if (state == State::executing) {
            if (control->state.compare_exchange_strong(
                    state,
                    State::completed,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                if (entry.periodic) {
                    decrement_pending_count();
                }
                m_index.erase(entry.id);
            }
            return;
        }

        if (state == State::cancelled) {
            m_index.erase(entry.id);
        }
    }

    void restore_unrun(std::vector<Entry>& batch, std::size_t first_unrun) {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (std::size_t i = batch.size(); i > first_unrun; --i) {
            auto& entry = batch[i - 1];
            auto state = State::ready_snapshot;
            if (!entry.control->state.compare_exchange_strong(
                    state,
                    State::queued_ready,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                continue;
            }

            m_ready[priority_index(entry.priority)].push_front(
                std::move(entry));
            increment_ready_count();
        }
    }

private:
    mutable std::mutex m_mutex;
    std::array<std::deque<Entry>, 3> m_ready;
    std::vector<Entry> m_delayed_heap;
    std::unordered_map<TaskId, std::shared_ptr<Control>> m_index;

    std::atomic<std::size_t> m_ready_count{0};
    std::atomic<std::size_t> m_pending_count{0};

    std::atomic<TaskId> m_next_id{1};
    std::atomic<std::uint64_t> m_next_seq{1};

    std::atomic<bool> m_closed{false};
    std::atomic<INotifier*> m_notifier{nullptr};

    mutable std::mutex m_exception_handler_mutex;
    std::shared_ptr<ExceptionHandler> m_exception_handler;
};

} // namespace event_hub

#endif // EVENT_HUB_TASK_MANAGER_HPP_INCLUDED
