#pragma once
#ifndef EVENT_HUB_TASK_HPP_INCLUDED
#define EVENT_HUB_TASK_HPP_INCLUDED

/// \file task.hpp
/// \brief Defines lightweight move-only task primitives.

#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace event_hub {

using TaskId = std::uint64_t; ///< Unique task identifier; zero is reserved for invalid or rejected submissions.

class TaskManager;

/// \class TaskContext
/// \brief Lightweight facade passed to context-aware task callbacks.
///
/// TaskContext is valid only while a task callback is running. Operations
/// return false after the task finishes or when the task state no longer allows
/// the requested change.
class TaskContext {
public:
    using Clock = std::chrono::steady_clock; ///< Monotonic clock used by TaskManager scheduling.
    using TimePoint = Clock::time_point; ///< Time point type used for rescheduling.
    using Duration = Clock::duration; ///< Duration type used for rescheduling.

    /// \brief Construct an inert context.
    TaskContext() noexcept = default;

    /// \brief Return the id of the running task, or zero for an inert context.
    TaskId id() const noexcept {
        return m_id;
    }

    /// \brief Cancel future work for the running task.
    bool cancel() noexcept {
        return m_cancel && m_owner ? m_cancel(m_owner, m_id) : false;
    }

    /// \brief Return true when the running task has been cancelled.
    bool is_cancelled() const noexcept {
        return m_is_cancelled && m_owner
                   ? m_is_cancelled(m_owner, m_id)
                   : false;
    }

    /// \brief Request the same task to run again at a steady-clock time point.
    bool reschedule_at(TimePoint due) noexcept {
        return m_reschedule && m_owner
                   ? m_reschedule(m_owner, m_id, due)
                   : false;
    }

    /// \brief Request the same task to run again after a delay.
    template <typename Rep, typename Period>
    bool reschedule_after(
        const std::chrono::duration<Rep, Period>& delay) noexcept {
        const auto due =
            Clock::now() + std::chrono::duration_cast<Duration>(delay);
        return reschedule_at(due);
    }

private:
    using CancelFn = bool (*)(void*, TaskId) noexcept;
    using IsCancelledFn = bool (*)(void*, TaskId) noexcept;
    using RescheduleFn = bool (*)(void*, TaskId, TimePoint) noexcept;

    TaskContext(void* owner,
                TaskId id,
                CancelFn cancel,
                IsCancelledFn is_cancelled,
                RescheduleFn reschedule) noexcept
        : m_owner(owner),
          m_id(id),
          m_cancel(cancel),
          m_is_cancelled(is_cancelled),
          m_reschedule(reschedule) {}

private:
    friend class TaskManager;

    void* m_owner = nullptr;
    TaskId m_id = 0;
    CancelFn m_cancel = nullptr;
    IsCancelledFn m_is_cancelled = nullptr;
    RescheduleFn m_reschedule = nullptr;
};

/// \brief Fixed task priority levels.
enum class TaskPriority : std::uint8_t {
    high = 0,
    normal = 1,
    low = 2
};

/// \brief Options used when submitting a task.
struct TaskOptions {
    TaskPriority priority{TaskPriority::normal}; ///< Priority used when the task becomes ready.
};

/// \brief Scheduling policy used by periodic tasks.
enum class PeriodicSchedule : std::uint8_t {
    fixed_delay = 0, ///< Wait one interval after each callback finishes.
    fixed_rate = 1 ///< Schedule each cycle from the previous planned deadline.
};

/// \brief Options used when submitting a periodic task.
struct PeriodicTaskOptions {
    TaskPriority priority{TaskPriority::normal}; ///< Priority used whenever a periodic cycle becomes ready.
    PeriodicSchedule schedule{PeriodicSchedule::fixed_delay}; ///< Timing policy used for cycles after the first run.
};

namespace detail {

template <typename F>
struct is_no_arg_task_callback
    : std::integral_constant<
          bool,
          std::is_invocable_r<void, typename std::decay<F>::type&>::value> {};

template <typename F>
struct is_context_task_callback
    : std::integral_constant<
          bool,
          std::is_invocable_r<void,
                              typename std::decay<F>::type&,
                              TaskContext&>::value> {};

template <typename F>
struct is_task_callback
    : std::integral_constant<bool,
                             is_no_arg_task_callback<F>::value ||
                                 is_context_task_callback<F>::value> {};

} // namespace detail

/// \class Task
/// \brief Move-only type-erased wrapper for a void() callable.
///
/// Unlike std::function<void()>, Task can hold move-only callables in C++17,
/// including lambdas that capture std::unique_ptr or std::packaged_task.
class Task {
public:
    /// \brief Construct an empty task.
    Task() noexcept = default;

    /// \brief Construct an empty task.
    Task(std::nullptr_t) noexcept {}

    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    /// \brief Construct from std::function for convenience.
    explicit Task(std::function<void()> fn) {
        if (fn) {
            assign_no_arg(std::move(fn));
        }
    }

    /// \brief Construct from a context-aware std::function for convenience.
    explicit Task(std::function<void(TaskContext&)> fn) {
        if (fn) {
            assign_context(std::move(fn));
        }
    }

    /// \brief Construct from any callable invocable as void().
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                !std::is_same<typename std::decay<F>::type,
                              std::nullptr_t>::value &&
                !std::is_same<typename std::decay<F>::type,
                              std::function<void()>>::value &&
                !std::is_same<typename std::decay<F>::type,
                              std::function<void(TaskContext&)>>::value &&
                std::is_invocable_r<void, typename std::decay<F>::type&>::value,
            int>::type = 0>
    Task(F&& fn) {
        assign_no_arg(std::forward<F>(fn));
    }

    /// \brief Construct from any callable invocable as void(TaskContext&).
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                !std::is_same<typename std::decay<F>::type,
                              std::nullptr_t>::value &&
                !std::is_same<typename std::decay<F>::type,
                              std::function<void()>>::value &&
                !std::is_same<typename std::decay<F>::type,
                              std::function<void(TaskContext&)>>::value &&
                !std::is_invocable_r<void,
                                     typename std::decay<F>::type&>::value &&
                std::is_invocable_r<void,
                                    typename std::decay<F>::type&,
                                    TaskContext&>::value,
            int>::type = 0>
    Task(F&& fn) {
        assign_context(std::forward<F>(fn));
    }

    /// \brief Return true when this task stores a callable.
    explicit operator bool() const noexcept {
        return static_cast<bool>(m_self);
    }

    /// \brief Invoke the stored callable.
    /// \throws std::bad_function_call when the task is empty.
    void operator()() {
        if (!m_self) {
            throw std::bad_function_call();
        }

        TaskContext context;
        m_self->call(context);
    }

    /// \brief Invoke the stored callable with a task context.
    /// \throws std::bad_function_call when the task is empty.
    void operator()(TaskContext& context) {
        if (!m_self) {
            throw std::bad_function_call();
        }

        m_self->call(context);
    }

    /// \brief Reset this task to empty.
    void reset() noexcept {
        m_self.reset();
    }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual void call(TaskContext& context) = 0;
    };

    template <typename F>
    struct NoArgModel final : Concept {
        template <typename U>
        explicit NoArgModel(U&& fn_)
            : fn(std::forward<U>(fn_)) {}

        void call(TaskContext&) override {
            fn();
        }

        F fn;
    };

    template <typename F>
    struct ContextModel final : Concept {
        template <typename U>
        explicit ContextModel(U&& fn_)
            : fn(std::forward<U>(fn_)) {}

        void call(TaskContext& context) override {
            fn(context);
        }

        F fn;
    };

    template <typename F>
    void assign_no_arg(F&& fn) {
        using Fn = typename std::decay<F>::type;
        m_self.reset(new NoArgModel<Fn>(std::forward<F>(fn)));
    }

    template <typename F>
    void assign_context(F&& fn) {
        using Fn = typename std::decay<F>::type;
        m_self.reset(new ContextModel<Fn>(std::forward<F>(fn)));
    }

private:
    std::unique_ptr<Concept> m_self;
};

} // namespace event_hub

#endif // EVENT_HUB_TASK_HPP_INCLUDED
