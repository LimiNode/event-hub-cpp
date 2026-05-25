#include <event_hub.hpp>

#include <iostream>
#include <string>

struct LogEvent {
    std::string text;
};

int main() {
    event_hub::SyncNotifier notifier;

    event_hub::EventBus bus;
    bus.set_notifier(&notifier);

    event_hub::TaskManager tasks;
    tasks.set_notifier(&notifier);

    event_hub::EventEndpoint endpoint(bus);
    bool running = true;

    endpoint.subscribe_queued<LogEvent>([&running](const LogEvent& event) {
        std::cout << "event: " << event.text << '\n';
        if (event.text == "quit") {
            running = false;
        }
    });

    tasks.post([] {
        std::cout << "task: immediate work\n";
    });

    tasks.post_after_ms(2, [&endpoint] {
        endpoint.post<LogEvent>("published by delayed task");
    });

    tasks.post_after_ms(4, [&endpoint] {
        endpoint.post<LogEvent>("quit");
    });

    while (running) {
        // Capture the shared wake-up generation before processing either
        // source. A bus or task notification that arrives while this loop is
        // busy will then make wait_for() return immediately.
        const auto generation = notifier.generation();

        std::size_t work_done = 0;
        // Process both passive sources on this application-owned thread.
        work_done += bus.process();
        // Limit task callbacks per iteration so the bus gets regular chances
        // to dispatch events even if producers keep filling the task queue.
        work_done += tasks.process(128);

        if (work_done != 0) {
            continue;
        }

        // Use 1 ms as the idle cap, but wake sooner for the next delayed task.
        const auto timeout = tasks.recommend_wait_for_ms(1);

        if (!bus.has_pending() && !tasks.has_ready()) {
            // Sleep only when both sources are idle. The old generation keeps
            // notifications between processing and waiting from being lost.
            (void)notifier.wait_for(generation, timeout);
        }
    }

    tasks.reset_notifier();
    bus.reset_notifier();
}
