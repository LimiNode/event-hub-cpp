#include <event_hub.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if EVENT_HUB_CPP_USE_TIME_SHIELD
#include <event_hub/calendar_scheduler.hpp>
#include <time_shield.hpp>
#endif

#if EVENT_HUB_CPP_USE_TIME_SHIELD && EVENT_HUB_CPP_USE_TIME_SHIELD_NTP
#include <time_shield/core/initialization.hpp>
#include <time_shield/ntp/ntp_time_service.hpp>
#endif

int main() {
#if EVENT_HUB_CPP_USE_TIME_SHIELD && EVENT_HUB_CPP_USE_TIME_SHIELD_NTP
    time_shield::init();

    if (!time_shield::ntp::init(30000, true)) {
        std::cout << "time-shield NTP service is not available now\n";
        return 0;
    }

    event_hub::TaskManager tasks;
    event_hub::CalendarScheduler calendar(tasks);
    std::vector<std::string> log;

    auto options = event_hub::CalendarTaskOptions::with_clock(
        time_shield::ZonedClock(time_shield::UTC, true));
    options.observer = [&log](const event_hub::CalendarObserverInfo& info) {
        if (info.event == event_hub::CalendarObserverEvent::scheduled) {
            log.push_back("scheduled by ntp utc ms " +
                          std::to_string(info.planned_utc_ms));
        }
    };

    const auto ntp_now_ms = time_shield::ntp::utc_time_ms();
    const auto due_dt =
        time_shield::DateTime::from_unix_ms(ntp_now_ms + time_shield::MS_PER_SEC)
            .to_date_time_struct_utc();
    const auto next_time =
        event_hub::time_of_day(due_dt.hour, due_dt.min, due_dt.sec);

    calendar.add_daily_task(
        next_time,
        [&log](event_hub::TaskContext& self) {
            log.push_back("daily task executed from NTP-backed clock");
            self.cancel();
        },
        options);

    const auto stop_at =
        event_hub::TaskManager::Clock::now() + std::chrono::seconds(2);
    while (tasks.has_pending() &&
           event_hub::TaskManager::Clock::now() < stop_at) {
        tasks.process();
        std::this_thread::sleep_for(tasks.recommend_wait_for_ms(50));
    }

    calendar.cancel_all();
    time_shield::ntp::shutdown();

    for (const auto& line : log) {
        std::cout << line << '\n';
    }

    return 0;
#elif EVENT_HUB_CPP_USE_TIME_SHIELD
    std::cout << "calendar_scheduler_ntp example requires "
                 "EVENT_HUB_CPP_USE_TIME_SHIELD_NTP=ON\n";
    return 0;
#else
    std::cout << "calendar_scheduler_ntp example requires "
                 "EVENT_HUB_CPP_USE_TIME_SHIELD=ON\n";
    return 0;
#endif
}
