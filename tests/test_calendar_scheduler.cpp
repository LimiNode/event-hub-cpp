#include "test_helpers.hpp"

#include <chrono>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if EVENT_HUB_CPP_USE_TIME_SHIELD
#include <event_hub/calendar_scheduler.hpp>
#include <time_shield.hpp>
#endif

int main() {
#if EVENT_HUB_CPP_USE_TIME_SHIELD
    auto ts_ms = [](time_shield::year_t year,
                    int month,
                    int day,
                    int hour,
                    int minute,
                    int second,
                    int millisecond = 0) {
        return time_shield::to_timestamp_ms(
            year,
            month,
            day,
            hour,
            minute,
            second,
            millisecond);
    };

    {
        auto options = event_hub::CalendarTaskOptions::with_clock(
                           time_shield::ZonedClock(time_shield::CET, true))
                           .use_now_provider([&] {
                               return ts_ms(2024, 10, 27, 0, 55, 0);
                           });

        EVENT_HUB_TEST_CHECK(options.clock.has_named_zone());
        EVENT_HUB_TEST_CHECK(options.clock.zone() == time_shield::CET);
        EVENT_HUB_TEST_CHECK(options.clock.use_ntp());
        EVENT_HUB_TEST_CHECK(options.now_provider() ==
                             ts_ms(2024, 10, 27, 0, 55, 0));

        auto fixed = event_hub::CalendarTaskOptions::fixed_utc_offset(
            static_cast<time_shield::tz_t>(time_shield::SEC_PER_HOUR));
        EVENT_HUB_TEST_CHECK(!fixed.clock.has_named_zone());
        EVENT_HUB_TEST_CHECK(fixed.clock.offset_at_utc_ms(0) ==
                             time_shield::SEC_PER_HOUR);
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        std::vector<event_hub::CalendarObserverInfo> events;

        event_hub::CalendarTaskOptions options;
        options.now_provider = [&] {
            return ts_ms(2024, 6, 1, 10, 0, 0);
        };
        options.observer = [&](const event_hub::CalendarObserverInfo& info) {
            events.push_back(info);
        };

        const auto id = calendar.add_daily_task(
            11 * static_cast<int>(time_shield::SEC_PER_HOUR),
            [] {},
            options);

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(events.size() >= 2);
        EVENT_HUB_TEST_CHECK(events[0].event ==
                             event_hub::CalendarObserverEvent::created);
        EVENT_HUB_TEST_CHECK(events[1].event ==
                             event_hub::CalendarObserverEvent::scheduled);
        EVENT_HUB_TEST_CHECK(events[1].planned_utc_ms ==
                             ts_ms(2024, 6, 1, 11, 0, 0));
        EVENT_HUB_TEST_CHECK(calendar.cancel(id));
    }

    {
        // Process immediate calendar tasks concurrently with publication of
        // their TaskManager ids. Every one-shot rule must still execute once;
        // an early consumer must not make add_rule() report failure.
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        std::atomic<bool> running{true};
        std::atomic<int> calls{0};
        std::thread consumer([&] {
            while (running.load(std::memory_order_relaxed)) {
                (void)tasks.process();
                std::this_thread::yield();
            }
            (void)tasks.process();
        });

        const auto now = ts_ms(2024, 6, 1, 10, 0, 0);
        constexpr int rule_count = 64;
        for (int i = 0; i != rule_count; ++i) {
            auto first = std::make_shared<bool>(true);
            event_hub::CalendarTaskOptions options;
            options.now_provider = [now] { return now; };
            const auto id = calendar.add_custom_calendar_task(
                [first](time_shield::ts_ms_t,
                        time_shield::ts_ms_t observed) mutable
                    -> std::optional<time_shield::ts_ms_t> {
                    if (*first) {
                        *first = false;
                        return observed;
                    }
                    return std::nullopt;
                },
                [&calls] { calls.fetch_add(1, std::memory_order_relaxed); },
                std::move(options));
            EVENT_HUB_TEST_CHECK(id != 0);
        }

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(2);
        while (calls.load(std::memory_order_relaxed) != rule_count &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        running.store(false, std::memory_order_relaxed);
        consumer.join();
        EVENT_HUB_TEST_CHECK(calls.load(std::memory_order_relaxed) == rule_count);
    }

    {
        const auto dinner_time = event_hub::time_of_day(18, 30);
        const auto precise_time = event_hub::time_of_day(18, 30, 15);
        const auto precise_time_ms = event_hub::time_of_day(18, 30, 15, 250);

        EVENT_HUB_TEST_CHECK(dinner_time.sec == 0);
        EVENT_HUB_TEST_CHECK(dinner_time.ms == 0);
        EVENT_HUB_TEST_CHECK(event_hub::is_valid_time_of_day(dinner_time));
        EVENT_HUB_TEST_CHECK(!event_hub::is_valid_time_of_day(
            event_hub::time_of_day(18, 30, 0, 1)));
        EVENT_HUB_TEST_CHECK(event_hub::second_of_day(dinner_time) ==
                             18 * static_cast<int>(time_shield::SEC_PER_HOUR) +
                                 30 *
                                     static_cast<int>(
                                         time_shield::SEC_PER_MIN));
        EVENT_HUB_TEST_CHECK(event_hub::second_of_day(precise_time) ==
                             18 * static_cast<int>(time_shield::SEC_PER_HOUR) +
                                 30 *
                                     static_cast<int>(
                                         time_shield::SEC_PER_MIN) +
                                 15);
        EVENT_HUB_TEST_CHECK(event_hub::ms_of_day(precise_time) ==
                             event_hub::second_of_day(precise_time) *
                                 static_cast<int>(time_shield::MS_PER_SEC));
        EVENT_HUB_TEST_CHECK(event_hub::ms_of_day(precise_time_ms) ==
                             event_hub::second_of_day(precise_time_ms) *
                                     static_cast<int>(
                                         time_shield::MS_PER_SEC) +
                                 250);

        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        std::vector<event_hub::CalendarObserverInfo> events;

        event_hub::CalendarTaskOptions options;
        options.now_provider = [&] {
            return ts_ms(2024, 6, 1, 10, 0, 0);
        };
        options.observer = [&](const event_hub::CalendarObserverInfo& info) {
            events.push_back(info);
        };

        const auto id = calendar.add_daily_task(
            precise_time,
            [] {},
            options);

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(events.size() >= 2);
        EVENT_HUB_TEST_CHECK(events[1].planned_utc_ms ==
                             ts_ms(2024, 6, 1, 18, 30, 15));
        EVENT_HUB_TEST_CHECK(calendar.cancel(id));
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        std::vector<event_hub::CalendarObserverInfo> events;

        event_hub::CalendarTaskOptions options;
        options.now_provider = [&] {
            return ts_ms(2024, 6, 3, 10, 0, 0);
        };
        options.observer = [&](const event_hub::CalendarObserverInfo& info) {
            events.push_back(info);
        };

        const auto id = calendar.add_weekly_task(
            event_hub::WED,
            9 * static_cast<int>(time_shield::SEC_PER_HOUR),
            [] {},
            options);

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(events[1].planned_utc_ms ==
                             ts_ms(2024, 6, 5, 9, 0, 0));
        EVENT_HUB_TEST_CHECK(calendar.cancel(id));
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        std::vector<event_hub::CalendarObserverInfo> events;

        event_hub::CalendarTaskOptions options;
        options.now_provider = [&] {
            return ts_ms(2024, 6, 3, 8, 0, 0);
        };
        options.observer = [&](const event_hub::CalendarObserverInfo& info) {
            events.push_back(info);
        };

        auto schedule =
            event_hub::WeeklySchedule{}
                .add(event_hub::MON, event_hub::time_of_day(9, 0))
                .add(event_hub::FRI, event_hub::time_of_day(18, 0));
        const auto id = calendar.add_weekly_task(std::move(schedule),
                                                 [] {},
                                                 options);

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(events[1].planned_utc_ms ==
                             ts_ms(2024, 6, 3, 9, 0, 0));
        EVENT_HUB_TEST_CHECK(calendar.cancel(id));
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        std::vector<event_hub::CalendarObserverInfo> events;

        event_hub::CalendarTaskOptions options;
        options.now_provider = [&] {
            return ts_ms(2024, 4, 30, 10, 0, 0);
        };
        options.observer = [&](const event_hub::CalendarObserverInfo& info) {
            events.push_back(info);
        };

        event_hub::MonthlySchedule schedule;
        schedule.add(31, 8 * static_cast<int>(time_shield::SEC_PER_HOUR));

        const auto id = calendar.add_monthly_task(std::move(schedule),
                                                  [] {},
                                                  options);

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(events[1].planned_utc_ms ==
                             ts_ms(2024, 5, 31, 8, 0, 0));
        EVENT_HUB_TEST_CHECK(calendar.cancel(id));
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        std::vector<event_hub::CalendarObserverInfo> events;

        event_hub::CalendarTaskOptions options;
        options.now_provider = [&] {
            return ts_ms(2024, 6, 1, 10, 0, 0);
        };
        options.observer = [&](const event_hub::CalendarObserverInfo& info) {
            events.push_back(info);
        };

        const auto id = calendar.add_monthly_task(
            15,
            event_hub::time_of_day(12, 0),
            [] {},
            options);

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(events[1].planned_utc_ms ==
                             ts_ms(2024, 6, 15, 12, 0, 0));
        EVENT_HUB_TEST_CHECK(calendar.cancel(id));
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        std::vector<event_hub::CalendarObserverInfo> events;

        event_hub::CalendarTaskOptions options;
        options.now_provider = [&] {
            return ts_ms(2024, 1, 3, 12, 0, 0);
        };
        options.last_due_utc_ms = ts_ms(2024, 1, 1, 10, 0, 0);
        options.observer = [&](const event_hub::CalendarObserverInfo& info) {
            events.push_back(info);
        };

        const auto id = calendar.add_daily_task(
            10 * static_cast<int>(time_shield::SEC_PER_HOUR),
            [] {},
            options);

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(events.size() >= 3);
        EVENT_HUB_TEST_CHECK(events[1].event ==
                             event_hub::CalendarObserverEvent::missed);
        EVENT_HUB_TEST_CHECK(events[1].planned_utc_ms ==
                             ts_ms(2024, 1, 2, 10, 0, 0));
        EVENT_HUB_TEST_CHECK(events[2].event ==
                             event_hub::CalendarObserverEvent::scheduled);
        EVENT_HUB_TEST_CHECK(events[2].planned_utc_ms ==
                             ts_ms(2024, 1, 4, 10, 0, 0));
        EVENT_HUB_TEST_CHECK(calendar.cancel(id));
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        std::vector<event_hub::CalendarObserverInfo> events;
        int calls = 0;

        event_hub::CalendarTaskOptions options;
        options.now_provider = [&] {
            return ts_ms(2024, 1, 3, 12, 0, 0);
        };
        options.last_due_utc_ms = ts_ms(2024, 1, 1, 10, 0, 0);
        options.missed_policy =
            event_hub::CalendarMissedRunPolicy::run_once_immediately;
        options.observer = [&](const event_hub::CalendarObserverInfo& info) {
            events.push_back(info);
        };

        const auto id = calendar.add_daily_task(
            10 * static_cast<int>(time_shield::SEC_PER_HOUR),
            [&calls](event_hub::TaskContext& self) {
                ++calls;
                self.cancel();
            },
            options);

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(tasks.ready_count() == 1);
        EVENT_HUB_TEST_CHECK(tasks.process() == 1);
        EVENT_HUB_TEST_CHECK(calls == 1);

        bool saw_due = false;
        for (const auto& event : events) {
            if (event.event == event_hub::CalendarObserverEvent::due &&
                event.planned_utc_ms == ts_ms(2024, 1, 2, 10, 0, 0)) {
                saw_due = true;
            }
        }
        EVENT_HUB_TEST_CHECK(saw_due);
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        std::vector<event_hub::CalendarObserverInfo> events;
        auto fake_now = ts_ms(2024, 1, 1, 9, 59, 59, 995);

        event_hub::CalendarTaskOptions options;
        options.now_provider = [&] {
            return fake_now;
        };
        options.observer = [&](const event_hub::CalendarObserverInfo& info) {
            events.push_back(info);
        };

        const auto id = calendar.add_daily_task(
            10 * static_cast<int>(time_shield::SEC_PER_HOUR),
            [&fake_now] {
                fake_now = time_shield::to_timestamp_ms(
                    2024,
                    1,
                    3,
                    12,
                    0,
                    0);
            },
            options);

        EVENT_HUB_TEST_CHECK(id != 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        EVENT_HUB_TEST_CHECK(tasks.process() == 1);

        bool saw_missed = false;
        bool saw_future = false;
        for (const auto& event : events) {
            if (event.event == event_hub::CalendarObserverEvent::missed &&
                event.planned_utc_ms == ts_ms(2024, 1, 2, 10, 0, 0)) {
                saw_missed = true;
            }
            if (event.event == event_hub::CalendarObserverEvent::scheduled &&
                event.planned_utc_ms == ts_ms(2024, 1, 4, 10, 0, 0)) {
                saw_future = true;
            }
        }

        EVENT_HUB_TEST_CHECK(saw_missed);
        EVENT_HUB_TEST_CHECK(saw_future);
        EVENT_HUB_TEST_CHECK(calendar.cancel(id));
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        std::vector<event_hub::CalendarObserverInfo> events;
        auto fake_now = ts_ms(2024, 1, 1, 9, 59, 59, 995);

        event_hub::CalendarTaskOptions options;
        options.now_provider = [&] {
            return fake_now;
        };
        options.overlap_policy =
            event_hub::CalendarOverlapPolicy::queue_one_after_current;
        options.observer = [&](const event_hub::CalendarObserverInfo& info) {
            events.push_back(info);
        };

        const auto id = calendar.add_daily_task(
            10 * static_cast<int>(time_shield::SEC_PER_HOUR),
            [&fake_now] {
                fake_now = time_shield::to_timestamp_ms(
                    2024,
                    1,
                    3,
                    12,
                    0,
                    0);
            },
            options);

        EVENT_HUB_TEST_CHECK(id != 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        EVENT_HUB_TEST_CHECK(tasks.process() == 1);
        EVENT_HUB_TEST_CHECK(tasks.ready_count() == 1);

        bool queued_overdue = false;
        for (const auto& event : events) {
            if (event.event == event_hub::CalendarObserverEvent::scheduled &&
                event.planned_utc_ms == ts_ms(2024, 1, 2, 10, 0, 0)) {
                queued_overdue = true;
            }
        }

        EVENT_HUB_TEST_CHECK(queued_overdue);
        EVENT_HUB_TEST_CHECK(calendar.cancel(id));
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        int calls = 0;

        const auto id = calendar.add_custom_calendar_task(
            [&calls](time_shield::ts_ms_t,
                     time_shield::ts_ms_t observed_now)
                -> std::optional<time_shield::ts_ms_t> {
                return calls == 0
                           ? std::optional<time_shield::ts_ms_t>(
                                 observed_now + 5)
                           : std::nullopt;
            },
            [&calls] {
                ++calls;
            });

        EVENT_HUB_TEST_CHECK(id != 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        EVENT_HUB_TEST_CHECK(tasks.process() == 1);
        EVENT_HUB_TEST_CHECK(calls == 1);
        EVENT_HUB_TEST_CHECK(!tasks.has_pending());
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        int calls = 0;
        int handled_errors = 0;

        tasks.set_exception_handler([&](std::exception_ptr error) {
            try {
                if (error) {
                    std::rethrow_exception(error);
                }
            } catch (const std::runtime_error&) {
                ++handled_errors;
            }
        });

        const auto id = calendar.add_custom_calendar_task(
            [](time_shield::ts_ms_t,
               time_shield::ts_ms_t observed_now)
                -> std::optional<time_shield::ts_ms_t> {
                return observed_now;
            },
            [&calls] {
                ++calls;
                throw std::runtime_error("calendar callback failed");
            });

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(tasks.ready_count() == 1);
        EVENT_HUB_TEST_CHECK(tasks.process() == 1);
        EVENT_HUB_TEST_CHECK(calls == 1);
        EVENT_HUB_TEST_CHECK(handled_errors == 1);
        EVENT_HUB_TEST_CHECK(!tasks.has_pending());
        EVENT_HUB_TEST_CHECK(!calendar.cancel(id));
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        int now_calls = 0;

        event_hub::CalendarTaskOptions options;
        options.now_provider = [&]() -> time_shield::ts_ms_t {
            ++now_calls;
            throw std::runtime_error("time source failed");
        };

        const auto id = calendar.add_daily_task(0, [] {}, options);

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(now_calls == 1);
        EVENT_HUB_TEST_CHECK(tasks.has_pending());
        EVENT_HUB_TEST_CHECK(calendar.cancel(id));
        EVENT_HUB_TEST_CHECK(!tasks.has_pending());
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        int provider_calls = 0;

        const auto id = calendar.add_custom_calendar_task(
            [&provider_calls](
                time_shield::ts_ms_t,
                time_shield::ts_ms_t) -> std::optional<time_shield::ts_ms_t> {
                ++provider_calls;
                throw std::runtime_error("calendar rule failed");
            },
            [] {});

        EVENT_HUB_TEST_CHECK(id == 0);
        EVENT_HUB_TEST_CHECK(provider_calls == 1);
        EVENT_HUB_TEST_CHECK(!tasks.has_pending());
        EVENT_HUB_TEST_CHECK(calendar.cancel_all() == 0);
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        std::vector<event_hub::CalendarObserverInfo> events;

        auto options = event_hub::CalendarTaskOptions::in_zone(
            time_shield::CET);
        options.now_provider = [&] {
            return ts_ms(2024, 3, 31, 0, 0, 0);
        };
        options.observer = [&](const event_hub::CalendarObserverInfo& info) {
            events.push_back(info);
        };

        const auto id = calendar.add_daily_task(
            2 * static_cast<int>(time_shield::SEC_PER_HOUR) +
                30 * static_cast<int>(time_shield::SEC_PER_MIN),
            [] {},
            options);

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(events.size() >= 2);
        EVENT_HUB_TEST_CHECK(events[1].planned_utc_ms ==
                             ts_ms(2024, 4, 1, 0, 30, 0));
        EVENT_HUB_TEST_CHECK(calendar.cancel(id));
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        std::vector<event_hub::CalendarObserverInfo> events;

        auto options = event_hub::CalendarTaskOptions::in_zone(
            time_shield::CET);
        options.now_provider = [&] {
            return ts_ms(2024, 10, 27, 0, 0, 0);
        };
        options.observer = [&](const event_hub::CalendarObserverInfo& info) {
            events.push_back(info);
        };

        const auto id = calendar.add_daily_task(
            2 * static_cast<int>(time_shield::SEC_PER_HOUR) +
                30 * static_cast<int>(time_shield::SEC_PER_MIN),
            [] {},
            options);

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(events.size() >= 2);
        EVENT_HUB_TEST_CHECK(events[1].planned_utc_ms ==
                             ts_ms(2024, 10, 27, 0, 30, 0));
        EVENT_HUB_TEST_CHECK(calendar.cancel(id));
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);
        std::vector<event_hub::CalendarObserverInfo> events;

        auto options = event_hub::CalendarTaskOptions::in_zone(
            time_shield::ET);
        options.now_provider = [&] {
            return ts_ms(2024, 3, 10, 6, 0, 0);
        };
        options.observer = [&](const event_hub::CalendarObserverInfo& info) {
            events.push_back(info);
        };

        const auto id = calendar.add_daily_task(
            2 * static_cast<int>(time_shield::SEC_PER_HOUR) +
                30 * static_cast<int>(time_shield::SEC_PER_MIN),
            [] {},
            options);

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(events.size() >= 2);
        EVENT_HUB_TEST_CHECK(events[1].planned_utc_ms ==
                             ts_ms(2024, 3, 11, 6, 30, 0));
        EVENT_HUB_TEST_CHECK(calendar.cancel(id));
    }

    {
        event_hub::TaskManager tasks;
        event_hub::CalendarScheduler calendar(tasks);

        EVENT_HUB_TEST_CHECK(calendar.add_daily_task(-1, [] {}) == 0);
        EVENT_HUB_TEST_CHECK(calendar.add_daily_task(
                                 event_hub::time_of_day(24, 0),
                                 [] {}) == 0);
        EVENT_HUB_TEST_CHECK(calendar.add_daily_task(
                                 event_hub::time_of_day(23, 60),
                                 [] {}) == 0);
        EVENT_HUB_TEST_CHECK(calendar.add_daily_task(
                                 event_hub::time_of_day(23, 59, 60),
                                 [] {}) == 0);
        EVENT_HUB_TEST_CHECK(calendar.add_daily_task(
                                 event_hub::time_of_day(23, 59, 59, 1),
                                 [] {}) == 0);

        event_hub::WeeklySchedule weekly;
        EVENT_HUB_TEST_CHECK(calendar.add_weekly_task(std::move(weekly),
                                                      [] {}) == 0);
        weekly.add(event_hub::MON, event_hub::time_of_day(9, 0, 0, 1));
        EVENT_HUB_TEST_CHECK(calendar.add_weekly_task(std::move(weekly),
                                                      [] {}) == 0);

        event_hub::MonthlySchedule monthly;
        EVENT_HUB_TEST_CHECK(calendar.add_monthly_task(std::move(monthly),
                                                       [] {}) == 0);
        monthly.add(0, event_hub::time_of_day(12, 0))
            .add(15, event_hub::time_of_day(12, 0, 0, 1));
        EVENT_HUB_TEST_CHECK(calendar.add_monthly_task(std::move(monthly),
                                                       [] {}) == 0);
        EVENT_HUB_TEST_CHECK(calendar.add_monthly_task(
                                 15,
                                 event_hub::time_of_day(12, 0, 0, 1),
                                 [] {}) == 0);

        event_hub::CalendarScheduler::NextTimeProvider empty_provider;
        EVENT_HUB_TEST_CHECK(calendar.add_custom_calendar_task(
                                 std::move(empty_provider),
                                 [] {}) == 0);
    }
#endif

    return 0;
}
