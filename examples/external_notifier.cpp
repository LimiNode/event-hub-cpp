#include <event_hub.hpp>

#include <chrono>
#include <iostream>
#include <string>

struct MessageEvent {
    std::string text;
};

int main() {
    event_hub::SyncNotifier notifier;

    event_hub::EventBus bus;
    bus.set_notifier(&notifier);

    event_hub::EventEndpoint endpoint(bus);
    endpoint.subscribe_queued<MessageEvent>([](const MessageEvent& event) {
        std::cout << "event: " << event.text << '\n';
    });

    event_hub::TaskManager tasks;
    tasks.set_notifier(&notifier);

    endpoint.post<MessageEvent>("configuration changed");
    tasks.post([] {
        std::cout << "task: refresh cache\n";
    });
    tasks.post_after(std::chrono::milliseconds(2), [] {
        std::cout << "task: delayed cleanup\n";
    });

    for (int tick = 0; tick != 5; ++tick) {
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
            (void)notifier.wait_for(generation, timeout);
        }
    }

    tasks.reset_notifier();
    bus.reset_notifier();
}
