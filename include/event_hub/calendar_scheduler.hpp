#pragma once
#ifndef EVENT_HUB_CALENDAR_SCHEDULER_HPP_INCLUDED
#define EVENT_HUB_CALENDAR_SCHEDULER_HPP_INCLUDED

/// \file calendar_scheduler.hpp
/// \brief Optional calendar-time scheduling layer over TaskManager.

#include "calendar_scheduler/detail.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace event_hub {

/// \class CalendarScheduler
/// \brief Calendar rule scheduler that delegates execution to TaskManager.
///
/// CalendarScheduler does not own a thread and never runs callbacks by itself.
/// It computes the next calendar occurrence, submits one TaskManager task, and
/// after that callback completes schedules the next occurrence. Calendar times
/// are represented as UTC Unix epoch milliseconds. Daily, weekly, and monthly
/// rules are interpreted in CalendarTaskOptions::clock, which defaults to UTC.
/// For named zones, nonexistent local times in DST gaps are skipped, and
/// ambiguous local times in DST folds are scheduled once at the first UTC
/// occurrence.
///
/// The TaskManager passed to the constructor must outlive this scheduler.
/// Destruction cancels queued one-shot tasks but does not wait for callbacks
/// that already started. Callbacks and observers that capture external objects
/// must guard those objects' lifetimes in user code.
///
/// If a calendar callback throws, the calendar rule is completed and will not
/// be scheduled again. The exception is propagated to TaskManager, so the
/// manager's exception handler policy decides whether process() reports it or
/// rethrows it.
class CalendarScheduler {
public:
    /// \brief Provider for custom calendar rules.
    ///
    /// The provider receives the exclusive UTC lower bound and observed UTC
    /// now, both in Unix epoch milliseconds, and returns the next planned UTC
    /// timestamp. Returning std::nullopt stops future scheduling.
    using NextTimeProvider = std::function<std::optional<time_shield::ts_ms_t>(
        time_shield::ts_ms_t after_utc_ms,
        time_shield::ts_ms_t observed_now_utc_ms)>;

    /// \brief Construct a scheduler over an existing TaskManager.
    ///
    /// \param tasks Task manager that must outlive this scheduler.
    explicit CalendarScheduler(TaskManager& tasks)
        : m_core(std::make_shared<calendar_scheduler_detail::Core>(&tasks)) {}

    /// \brief Cancel queued calendar tasks owned by this scheduler.
    ///
    /// Does not wait for callbacks that already started. Cancelled observer
    /// events may be emitted during destruction; observer exceptions are
    /// ignored.
    ~CalendarScheduler() {
        cancel_all_on_core(m_core, true);
    }

    CalendarScheduler(const CalendarScheduler&) = delete;
    CalendarScheduler& operator=(const CalendarScheduler&) = delete;
    CalendarScheduler(CalendarScheduler&&) = delete;
    CalendarScheduler& operator=(CalendarScheduler&&) = delete;

    /// \brief Add a daily task at second_of_day in the configured local zone.
    CalendarTaskId add_daily_task(int second_of_day,
                                  Task callback,
                                  CalendarTaskOptions options = {}) {
        if (!callback ||
            !calendar_scheduler_detail::valid_second_of_day(second_of_day)) {
            return 0;
        }

        std::vector<int> seconds{second_of_day};
        return add_rule(
            calendar_scheduler_detail::make_daily_provider(
                calendar_scheduler_detail::normalize_seconds(
                    std::move(seconds))),
            std::move(callback),
            std::move(options));
    }

    /// \brief Add a daily callable at second_of_day.
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    CalendarTaskId add_daily_task(int second_of_day,
                                  F&& callback,
                                  CalendarTaskOptions options = {}) {
        return add_daily_task(second_of_day,
                              Task(std::forward<F>(callback)),
                              std::move(options));
    }

    /// \brief Add a daily task at a local time in the configured zone.
    CalendarTaskId add_daily_task(LocalTime time,
                                  Task callback,
                                  CalendarTaskOptions options = {}) {
        if (!is_valid_time_of_day(time)) {
            return 0;
        }

        return add_daily_task(second_of_day(time),
                              std::move(callback),
                              std::move(options));
    }

    /// \brief Add a daily callable at a local time.
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    CalendarTaskId add_daily_task(LocalTime time,
                                  F&& callback,
                                  CalendarTaskOptions options = {}) {
        return add_daily_task(time,
                              Task(std::forward<F>(callback)),
                              std::move(options));
    }

    /// \brief Add a weekly task for one weekday and second of day.
    CalendarTaskId add_weekly_task(time_shield::Weekday day_of_week,
                                   int second_of_day,
                                   Task callback,
                                   CalendarTaskOptions options = {}) {
        if (!callback ||
            !calendar_scheduler_detail::valid_weekday(day_of_week) ||
            !calendar_scheduler_detail::valid_second_of_day(second_of_day)) {
            return 0;
        }

        WeeklySchedule schedule;
        schedule.seconds_by_weekday[static_cast<std::size_t>(day_of_week)]
            .push_back(second_of_day);
        return add_weekly_task(std::move(schedule),
                               std::move(callback),
                               std::move(options));
    }

    /// \brief Add a weekly callable for one weekday and second of day.
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    CalendarTaskId add_weekly_task(time_shield::Weekday day_of_week,
                                   int second_of_day,
                                   F&& callback,
                                   CalendarTaskOptions options = {}) {
        return add_weekly_task(day_of_week,
                               second_of_day,
                               Task(std::forward<F>(callback)),
                               std::move(options));
    }

    /// \brief Add a weekly task for one weekday and local time.
    CalendarTaskId add_weekly_task(time_shield::Weekday day_of_week,
                                   LocalTime time,
                                   Task callback,
                                   CalendarTaskOptions options = {}) {
        if (!is_valid_time_of_day(time)) {
            return 0;
        }

        return add_weekly_task(day_of_week,
                               second_of_day(time),
                               std::move(callback),
                               std::move(options));
    }

    /// \brief Add a weekly callable for one weekday and local time.
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    CalendarTaskId add_weekly_task(time_shield::Weekday day_of_week,
                                   LocalTime time,
                                   F&& callback,
                                   CalendarTaskOptions options = {}) {
        return add_weekly_task(day_of_week,
                               time,
                               Task(std::forward<F>(callback)),
                               std::move(options));
    }

    /// \brief Add a weekly task with per-weekday seconds-of-day lists.
    CalendarTaskId add_weekly_task(WeeklySchedule schedule,
                                   Task callback,
                                   CalendarTaskOptions options = {}) {
        if (!callback || !calendar_scheduler_detail::normalize_weekly(
                             schedule)) {
            return 0;
        }

        return add_rule(calendar_scheduler_detail::make_weekly_provider(
                            std::move(schedule)),
                        std::move(callback),
                        std::move(options));
    }

    /// \brief Add a weekly callable with per-weekday seconds-of-day lists.
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    CalendarTaskId add_weekly_task(WeeklySchedule schedule,
                                   F&& callback,
                                   CalendarTaskOptions options = {}) {
        return add_weekly_task(std::move(schedule),
                               Task(std::forward<F>(callback)),
                               std::move(options));
    }

    /// \brief Add a monthly task with per-month-day seconds-of-day lists.
    CalendarTaskId add_monthly_task(MonthlySchedule schedule,
                                    Task callback,
                                    CalendarTaskOptions options = {}) {
        if (!callback || !calendar_scheduler_detail::normalize_monthly(
                             schedule)) {
            return 0;
        }

        return add_rule(calendar_scheduler_detail::make_monthly_provider(
                            std::move(schedule)),
                        std::move(callback),
                        std::move(options));
    }

    /// \brief Add a monthly callable with per-month-day seconds-of-day lists.
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    CalendarTaskId add_monthly_task(MonthlySchedule schedule,
                                    F&& callback,
                                    CalendarTaskOptions options = {}) {
        return add_monthly_task(std::move(schedule),
                                Task(std::forward<F>(callback)),
                                std::move(options));
    }

    /// \brief Add a monthly task for one month day and local time.
    CalendarTaskId add_monthly_task(int month_day,
                                    LocalTime time,
                                    Task callback,
                                    CalendarTaskOptions options = {}) {
        if (!is_valid_time_of_day(time)) {
            return 0;
        }

        MonthlySchedule schedule;
        schedule.add(month_day, time);
        return add_monthly_task(std::move(schedule),
                                std::move(callback),
                                std::move(options));
    }

    /// \brief Add a monthly callable for one month day and local time.
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    CalendarTaskId add_monthly_task(int month_day,
                                    LocalTime time,
                                    F&& callback,
                                    CalendarTaskOptions options = {}) {
        return add_monthly_task(month_day,
                                time,
                                Task(std::forward<F>(callback)),
                                std::move(options));
    }

    /// \brief Add a task whose next UTC time is supplied by user code.
    CalendarTaskId add_custom_calendar_task(NextTimeProvider next_time_provider,
                                            Task callback,
                                            CalendarTaskOptions options = {}) {
        if (!next_time_provider || !callback) {
            return 0;
        }

        return add_rule(
            [provider = std::move(next_time_provider)](
                const CalendarTaskOptions&,
                time_shield::ts_ms_t after_utc_ms,
                time_shield::ts_ms_t observed_now_utc_ms)
                -> std::optional<time_shield::ts_ms_t> {
                return provider(after_utc_ms, observed_now_utc_ms);
            },
            std::move(callback),
            std::move(options));
    }

    /// \brief Add a custom calendar callable.
    template <
        typename F,
        typename std::enable_if<
            !std::is_same<typename std::decay<F>::type, Task>::value &&
                detail::is_task_callback<F>::value,
            int>::type = 0>
    CalendarTaskId add_custom_calendar_task(NextTimeProvider next_time_provider,
                                            F&& callback,
                                            CalendarTaskOptions options = {}) {
        return add_custom_calendar_task(std::move(next_time_provider),
                                        Task(std::forward<F>(callback)),
                                        std::move(options));
    }

    /// \brief Cancel a calendar rule and its queued one-shot task.
    bool cancel(CalendarTaskId id) noexcept {
        return cancel_on_core(m_core, id);
    }

    /// \brief Cancel all active calendar rules owned by this scheduler.
    std::size_t cancel_all() noexcept {
        return cancel_all_on_core(m_core, false);
    }

private:
    using RuleProvider = calendar_scheduler_detail::RuleProvider;
    using Core = calendar_scheduler_detail::Core;
    using State = calendar_scheduler_detail::State;
    using PlannedDue = calendar_scheduler_detail::PlannedDue;

    CalendarTaskId add_rule(RuleProvider provider,
                            Task callback,
                            CalendarTaskOptions options) {
        auto core = m_core;
        if (!core || !provider || !callback) {
            return 0;
        }

        auto state = std::make_shared<State>();
        state->options = std::move(options);
        state->next_time_provider = std::move(provider);
        state->callback = std::move(callback);

        {
            std::lock_guard<std::mutex> lock(core->mutex);
            if (core->closed || !core->manager ||
                core->manager->is_closed()) {
                return 0;
            }

            state->id = calendar_scheduler_detail::next_calendar_id(*core);
            core->tasks.emplace(state->id, state);
        }

        const auto observed_now =
            calendar_scheduler_detail::safe_now_utc_ms(state->options);
        emit(state, CalendarObserverEvent::created, 0, observed_now, 0);

        std::optional<PlannedDue> planned;
        try {
            planned = first_planned_due(*state, observed_now);
        } catch (...) {
            cancel_on_core(core, state->id);
            return 0;
        }
        if (!planned) {
            cancel_on_core(core, state->id);
            return 0;
        }

        if (!schedule_one(core,
                          state,
                          planned->planned_utc_ms,
                          observed_now,
                          planned->immediate)) {
            cancel_on_core(core, state->id);
            return 0;
        }

        return state->id;
    }

    static std::optional<PlannedDue> first_planned_due(
        State& state,
        time_shield::ts_ms_t observed_now) {
        if (state.options.last_due_utc_ms) {
            const auto missed = state.next_time_provider(
                state.options,
                *state.options.last_due_utc_ms,
                observed_now);
            if (!missed) {
                return std::nullopt;
            }

            if (*missed <= observed_now) {
                emit(state,
                     CalendarObserverEvent::missed,
                     *missed,
                     observed_now,
                     0);

                if (state.options.missed_policy ==
                    CalendarMissedRunPolicy::run_once_immediately) {
                    return PlannedDue{*missed, true};
                }

                auto future = state.next_time_provider(state.options,
                                                       observed_now,
                                                       observed_now);
                if (!future) {
                    return std::nullopt;
                }
                return PlannedDue{*future, *future <= observed_now};
            }

            return PlannedDue{*missed, false};
        }

        auto planned = state.next_time_provider(state.options,
                                                observed_now,
                                                observed_now);
        if (!planned) {
            return std::nullopt;
        }

        return PlannedDue{*planned, *planned <= observed_now};
    }

    static bool schedule_one(std::shared_ptr<Core> core,
                             std::shared_ptr<State> state,
                             time_shield::ts_ms_t planned_utc_ms,
                             time_shield::ts_ms_t observed_now,
                             bool immediate) {
        if (!core || !state) {
            return false;
        }

        const auto delay_ms =
            immediate || planned_utc_ms <= observed_now
                ? time_shield::ts_ms_t{0}
                : planned_utc_ms - observed_now;
        const auto steady_due =
            TaskManager::Clock::now() + std::chrono::milliseconds(delay_ms);

        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(core->mutex);
            auto it = core->tasks.find(state->id);
            if (it == core->tasks.end() || state->cancelled || core->closed) {
                return false;
            }
            generation = ++state->schedule_generation;
            state->planned_utc_ms = planned_utc_ms;
            state->scheduled_task_id = 0;
            state->started_generation = 0;
        }

        std::weak_ptr<Core> weak_core = core;
        std::weak_ptr<State> weak_state = state;
        auto task = Task([weak_core, weak_state, generation](TaskContext& context) {
            auto locked_core = weak_core.lock();
            auto locked_state = weak_state.lock();
            if (!locked_core || !locked_state) {
                return;
            }

            run_due(std::move(locked_core),
                    std::move(locked_state),
                    generation,
                    context);
        });

        const auto task_id = core->manager->add_task_at(
            steady_due,
            std::move(task),
            TaskOptions{state->options.priority});
        if (task_id == 0) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(core->mutex);
            // A concurrent consumer may already have run this generation and
            // advanced the schedule (or completed it). This is a successful
            // publication, not a failure of the rule being scheduled.
            if (state->schedule_generation != generation ||
                state->started_generation == generation) {
                return true;
            }

            auto it = core->tasks.find(state->id);
            if (it == core->tasks.end() || state->cancelled || core->closed) {
                core->manager->cancel(task_id);
                return false;
            }

            if (state->scheduled_task_id == 0) {
                state->scheduled_task_id = task_id;
            }
        }

        emit(state,
             CalendarObserverEvent::scheduled,
             planned_utc_ms,
             observed_now,
             task_id);
        return true;
    }

    static void run_due(std::shared_ptr<Core> core,
                        std::shared_ptr<State> state,
                        std::uint64_t generation,
                        TaskContext& context) {
        time_shield::ts_ms_t planned = 0;
        TaskId task_id = context.id();
        std::uint64_t run_count = 0;
        {
            std::lock_guard<std::mutex> lock(core->mutex);
            auto it = core->tasks.find(state->id);
            if (it == core->tasks.end() || state->cancelled || core->closed ||
                state->schedule_generation != generation) {
                return;
            }

            planned = state->planned_utc_ms;
            task_id = context.id();
            state->scheduled_task_id = task_id;
            state->started_generation = generation;
            run_count = state->run_count;
        }

        const auto observed_now =
            calendar_scheduler_detail::safe_now_utc_ms(state->options);
        emit(state,
             CalendarObserverEvent::due,
             planned,
             observed_now,
             task_id,
             run_count);
        emit(state,
             CalendarObserverEvent::before_callback,
             planned,
             observed_now,
             task_id,
             run_count);

        try {
            state->callback(context);
        } catch (...) {
            emit(state,
                 CalendarObserverEvent::after_callback,
                 planned,
                 calendar_scheduler_detail::safe_now_utc_ms(state->options),
                 task_id,
                 run_count);
            mark_completed(core, state);
            throw;
        }

        const auto after_now =
            calendar_scheduler_detail::safe_now_utc_ms(state->options);
        {
            std::lock_guard<std::mutex> lock(core->mutex);
            auto it = core->tasks.find(state->id);
            if (it == core->tasks.end() || state->cancelled ||
                core->closed || context.is_cancelled()) {
                state->cancelled = true;
            } else {
                ++state->run_count;
                run_count = state->run_count;
            }
        }

        emit(state,
             CalendarObserverEvent::after_callback,
             planned,
             after_now,
             task_id,
             run_count);

        if (context.is_cancelled() || is_state_cancelled(core, state->id)) {
            mark_completed(core, state);
            emit(state,
                 CalendarObserverEvent::cancelled,
                 planned,
                 after_now,
                 task_id,
                 run_count);
            return;
        }

        schedule_after_run(core, state, planned, after_now);
    }

    static void schedule_after_run(std::shared_ptr<Core> core,
                                   std::shared_ptr<State> state,
                                   time_shield::ts_ms_t previous_planned,
                                   time_shield::ts_ms_t observed_now) {
        std::optional<time_shield::ts_ms_t> next;
        try {
            next = state->next_time_provider(state->options,
                                             previous_planned,
                                             observed_now);
        } catch (...) {
            // A provider failure terminates this rule. Keep the exception
            // visible to TaskManager's normal error handling, but do not
            // leave an orphaned rule in Core with no queued task.
            mark_completed(core, state);
            throw;
        }
        if (!next) {
            mark_completed(core, state);
            return;
        }

        bool immediate = false;
        if (*next <= observed_now) {
            emit(state,
                 CalendarObserverEvent::missed,
                 *next,
                 observed_now,
                 0);

            if (state->options.overlap_policy ==
                CalendarOverlapPolicy::queue_one_after_current) {
                immediate = true;
            } else {
                std::optional<time_shield::ts_ms_t> future;
                try {
                    future = state->next_time_provider(state->options,
                                                       observed_now,
                                                       observed_now);
                } catch (...) {
                    mark_completed(core, state);
                    throw;
                }
                if (!future) {
                    mark_completed(core, state);
                    return;
                }

                next = future;
                immediate = *next <= observed_now;
            }
        }

        if (!schedule_one(core, state, *next, observed_now, immediate)) {
            mark_completed(core, state);
        }
    }

    static bool is_state_cancelled(std::shared_ptr<Core> core,
                                   CalendarTaskId id) {
        std::lock_guard<std::mutex> lock(core->mutex);
        auto it = core->tasks.find(id);
        return it == core->tasks.end() || it->second->cancelled ||
               core->closed;
    }

    static void mark_completed(std::shared_ptr<Core> core,
                               std::shared_ptr<State> state) {
        if (!core || !state) {
            return;
        }

        std::lock_guard<std::mutex> lock(core->mutex);
        state->cancelled = true;
        core->tasks.erase(state->id);
    }

    static bool cancel_on_core(std::shared_ptr<Core> core,
                               CalendarTaskId id) noexcept {
        if (!core || id == 0) {
            return false;
        }

        std::shared_ptr<State> state;
        TaskId task_id = 0;
        std::uint64_t run_count = 0;
        time_shield::ts_ms_t planned = 0;
        {
            std::lock_guard<std::mutex> lock(core->mutex);
            auto it = core->tasks.find(id);
            if (it == core->tasks.end() || it->second->cancelled) {
                return false;
            }

            state = it->second;
            state->cancelled = true;
            task_id = state->scheduled_task_id;
            planned = state->planned_utc_ms;
            run_count = state->run_count;
            core->tasks.erase(it);
        }

        if (task_id != 0 && core->manager) {
            core->manager->cancel(task_id);
        }

        emit(state,
             CalendarObserverEvent::cancelled,
             planned,
             calendar_scheduler_detail::safe_now_utc_ms(state->options),
             task_id,
             run_count);
        return true;
    }

    static std::size_t cancel_all_on_core(std::shared_ptr<Core> core,
                                          bool close_core) noexcept {
        if (!core) {
            return 0;
        }

        std::vector<std::shared_ptr<State>> states;
        {
            std::lock_guard<std::mutex> lock(core->mutex);
            if (close_core && core->closed && core->tasks.empty()) {
                return 0;
            }

            if (close_core) {
                core->closed = true;
            }

            states.reserve(core->tasks.size());
            for (auto& item : core->tasks) {
                item.second->cancelled = true;
                states.push_back(item.second);
            }
            core->tasks.clear();
        }

        for (auto& state : states) {
            if (state->scheduled_task_id != 0 && core->manager) {
                core->manager->cancel(state->scheduled_task_id);
            }

            emit(state,
                 CalendarObserverEvent::cancelled,
                 state->planned_utc_ms,
                 calendar_scheduler_detail::safe_now_utc_ms(state->options),
                 state->scheduled_task_id,
                 state->run_count);
        }

        return states.size();
    }

    static void emit(const std::shared_ptr<State>& state,
                     CalendarObserverEvent event,
                     time_shield::ts_ms_t planned,
                     time_shield::ts_ms_t observed,
                     TaskId scheduled_task_id,
                     std::uint64_t run_count) noexcept {
        if (!state || !state->options.observer) {
            return;
        }

        CalendarObserverInfo info;
        info.id = state->id;
        info.scheduled_task_id = scheduled_task_id;
        info.planned_utc_ms = planned;
        info.observed_utc_ms = observed;
        info.event = event;
        info.run_count = run_count;

        try {
            state->options.observer(info);
        } catch (...) {
        }
    }

    static void emit(const State& state,
                     CalendarObserverEvent event,
                     time_shield::ts_ms_t planned,
                     time_shield::ts_ms_t observed,
                     TaskId scheduled_task_id) noexcept {
        if (!state.options.observer) {
            return;
        }

        CalendarObserverInfo info;
        info.id = state.id;
        info.scheduled_task_id = scheduled_task_id;
        info.planned_utc_ms = planned;
        info.observed_utc_ms = observed;
        info.event = event;
        info.run_count = state.run_count;

        try {
            state.options.observer(info);
        } catch (...) {
        }
    }

    static void emit(const std::shared_ptr<State>& state,
                     CalendarObserverEvent event,
                     time_shield::ts_ms_t planned,
                     time_shield::ts_ms_t observed,
                     TaskId scheduled_task_id) noexcept {
        if (!state) {
            return;
        }
        emit(*state, event, planned, observed, scheduled_task_id);
    }

private:
    std::shared_ptr<Core> m_core;
};

} // namespace event_hub

#endif // EVENT_HUB_CALENDAR_SCHEDULER_HPP_INCLUDED
