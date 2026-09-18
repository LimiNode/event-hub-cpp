#pragma once
#ifndef EVENT_HUB_CALENDAR_SCHEDULER_DETAIL_HPP_INCLUDED
#define EVENT_HUB_CALENDAR_SCHEDULER_DETAIL_HPP_INCLUDED

/// \file calendar_scheduler/detail.hpp
/// \brief Internal helpers for CalendarScheduler.

#include "types.hpp"

#include "../task_manager.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace event_hub {
namespace calendar_scheduler_detail {

using RuleProvider = std::function<std::optional<time_shield::ts_ms_t>(
    const CalendarTaskOptions& options,
    time_shield::ts_ms_t after_utc_ms,
    time_shield::ts_ms_t observed_now_utc_ms)>;

struct State {
    CalendarTaskId id = 0;
    CalendarTaskOptions options;
    RuleProvider next_time_provider;
    Task callback;
    TaskId scheduled_task_id = 0;
    time_shield::ts_ms_t planned_utc_ms = 0;
    std::uint64_t schedule_generation = 0;
    std::uint64_t started_generation = 0;
    std::uint64_t run_count = 0;
    bool cancelled = false;
};

struct Core {
    explicit Core(TaskManager* manager_) noexcept
        : manager(manager_) {}

    TaskManager* manager = nullptr;
    std::mutex mutex;
    std::unordered_map<CalendarTaskId, std::shared_ptr<State>> tasks;
    std::atomic<CalendarTaskId> next_id{1};
    bool closed = false;
};

struct PlannedDue {
    time_shield::ts_ms_t planned_utc_ms = 0;
    bool immediate = false;
};

inline bool valid_second_of_day(int second) noexcept {
    return second >= 0 && second <= time_shield::MAX_SEC_PER_DAY;
}

inline bool valid_weekday(time_shield::Weekday day) noexcept {
    return day >= time_shield::SUN && day <= time_shield::SAT;
}

inline std::vector<int> normalize_seconds(std::vector<int> seconds) {
    seconds.erase(std::remove_if(seconds.begin(),
                                 seconds.end(),
                                 [](int second) {
                                     return !valid_second_of_day(second);
                                 }),
                  seconds.end());
    std::sort(seconds.begin(), seconds.end());
    seconds.erase(std::unique(seconds.begin(), seconds.end()), seconds.end());
    return seconds;
}

inline bool normalize_weekly(WeeklySchedule& schedule) {
    bool has_any = false;
    for (auto& seconds : schedule.seconds_by_weekday) {
        seconds = normalize_seconds(std::move(seconds));
        has_any = has_any || !seconds.empty();
    }
    return has_any;
}

inline bool normalize_monthly(MonthlySchedule& schedule) {
    bool has_any = false;
    schedule.seconds_by_month_day[0].clear();
    for (std::size_t day = 1; day != schedule.seconds_by_month_day.size();
         ++day) {
        auto& seconds = schedule.seconds_by_month_day[day];
        seconds = normalize_seconds(std::move(seconds));
        has_any = has_any || !seconds.empty();
    }
    return has_any;
}

inline CalendarTaskId next_calendar_id(Core& core) noexcept {
    auto id = core.next_id.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) {
        id = core.next_id.fetch_add(1, std::memory_order_relaxed);
    }
    return id;
}

inline time_shield::ts_ms_t now_utc_ms(const CalendarTaskOptions& options) {
    return options.now_provider ? options.now_provider()
                                : options.clock.utc_time_ms();
}

inline time_shield::ts_ms_t safe_now_utc_ms(
    const CalendarTaskOptions& options) noexcept {
    try {
        return now_utc_ms(options);
    } catch (...) {
        return options.clock.utc_time_ms();
    }
}

inline time_shield::ts_ms_t utc_to_local_ms(
    const CalendarTaskOptions& options,
    time_shield::ts_ms_t utc_ms) {
    if (options.clock.has_named_zone()) {
        return time_shield::gmt_to_zone_ms(utc_ms, options.clock.zone());
    }

    return utc_ms + static_cast<time_shield::ts_ms_t>(
                        options.clock.offset_at_utc_ms(utc_ms)) *
                        time_shield::MS_PER_SEC;
}

inline time_shield::ts_ms_t local_to_utc_ms(
    const CalendarTaskOptions& options,
    time_shield::ts_ms_t local_ms) {
    if (options.clock.has_named_zone()) {
        return time_shield::zone_to_gmt_ms(
            local_ms,
            options.clock.zone(),
            time_shield::AmbiguousTimePolicy::first_occurrence,
            time_shield::NonexistentTimePolicy::error);
    }

    return local_ms - static_cast<time_shield::ts_ms_t>(
                          options.clock.offset_at_utc_ms(0)) *
                          time_shield::MS_PER_SEC;
}

inline time_shield::DateTimeStruct local_struct_from_ms(
    time_shield::ts_ms_t local_ms) {
    return time_shield::DateTime::from_unix_ms(local_ms)
        .to_date_time_struct_utc();
}

inline time_shield::ts_ms_t local_day_start_ms(
    time_shield::ts_ms_t local_ms) {
    const auto dt = local_struct_from_ms(local_ms);
    return time_shield::to_timestamp_ms(dt.year, dt.mon, dt.day);
}

template <typename DayAllowed>
std::optional<time_shield::ts_ms_t> scan_local_days(
    const CalendarTaskOptions& options,
    time_shield::ts_ms_t after_utc_ms,
    int max_days,
    DayAllowed&& day_allowed,
    const std::function<const std::vector<int>&(
        const time_shield::DateTimeStruct&)>& seconds_for_day) {
    const auto local_after = utc_to_local_ms(options, after_utc_ms);
    auto day_start = local_day_start_ms(local_after);

    for (int offset = 0; offset <= max_days; ++offset) {
        const auto current_day =
            day_start +
            static_cast<time_shield::ts_ms_t>(offset) *
                time_shield::MS_PER_DAY;
        const auto dt = local_struct_from_ms(current_day);
        if (!day_allowed(dt)) {
            continue;
        }

        const auto& seconds = seconds_for_day(dt);
        for (const auto second : seconds) {
            const auto local_due =
                current_day +
                static_cast<time_shield::ts_ms_t>(second) *
                    time_shield::MS_PER_SEC;
            const auto utc_due = local_to_utc_ms(options, local_due);
            if (utc_due == time_shield::ERROR_TIMESTAMP) {
                continue;
            }
            if (utc_due > after_utc_ms) {
                return utc_due;
            }
        }
    }

    return std::nullopt;
}

inline RuleProvider make_daily_provider(std::vector<int> seconds) {
    return [seconds = std::move(seconds)](
               const CalendarTaskOptions& options,
               time_shield::ts_ms_t after_utc_ms,
               time_shield::ts_ms_t)
               -> std::optional<time_shield::ts_ms_t> {
        const auto seconds_for_day =
            [&seconds](const time_shield::DateTimeStruct&)
            -> const std::vector<int>& {
            return seconds;
        };

        return scan_local_days(options,
                               after_utc_ms,
                               2,
                               [](const time_shield::DateTimeStruct&) {
                                   return true;
                               },
                               seconds_for_day);
    };
}

inline RuleProvider make_weekly_provider(WeeklySchedule schedule) {
    return [schedule = std::move(schedule)](
               const CalendarTaskOptions& options,
               time_shield::ts_ms_t after_utc_ms,
               time_shield::ts_ms_t)
               -> std::optional<time_shield::ts_ms_t> {
        const auto day_allowed =
            [&schedule](const time_shield::DateTimeStruct& dt) {
                const auto weekday =
                    time_shield::day_of_week_date<time_shield::Weekday>(
                        dt.year,
                        dt.mon,
                        dt.day);
                return !schedule
                            .seconds_by_weekday[static_cast<std::size_t>(
                                weekday)]
                            .empty();
            };
        const auto seconds_for_day =
            [&schedule](const time_shield::DateTimeStruct& dt)
            -> const std::vector<int>& {
            const auto weekday =
                time_shield::day_of_week_date<time_shield::Weekday>(
                    dt.year,
                    dt.mon,
                    dt.day);
            return schedule.seconds_by_weekday[static_cast<std::size_t>(
                weekday)];
        };

        return scan_local_days(options,
                               after_utc_ms,
                               14,
                               day_allowed,
                               seconds_for_day);
    };
}

inline RuleProvider make_monthly_provider(MonthlySchedule schedule) {
    return [schedule = std::move(schedule)](
               const CalendarTaskOptions& options,
               time_shield::ts_ms_t after_utc_ms,
               time_shield::ts_ms_t)
               -> std::optional<time_shield::ts_ms_t> {
        const auto day_allowed =
            [&schedule](const time_shield::DateTimeStruct& dt) {
                return dt.day >= 1 &&
                       dt.day <
                           static_cast<int>(
                               schedule.seconds_by_month_day.size()) &&
                       !schedule.seconds_by_month_day[dt.day].empty();
            };
        const auto seconds_for_day =
            [&schedule](const time_shield::DateTimeStruct& dt)
            -> const std::vector<int>& {
            return schedule.seconds_by_month_day[dt.day];
        };

        return scan_local_days(options,
                               after_utc_ms,
                               370,
                               day_allowed,
                               seconds_for_day);
    };
}

} // namespace calendar_scheduler_detail
} // namespace event_hub

#endif // EVENT_HUB_CALENDAR_SCHEDULER_DETAIL_HPP_INCLUDED
