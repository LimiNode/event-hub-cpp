#include <event_hub.hpp>

#include <chrono>
#include <iostream>
#include <string>

struct LogEvent {
    std::string text;
};

int main() {
    using namespace std::chrono_literals;

    event_hub::EventBus bus;
    event_hub::EventEndpoint endpoint(bus);
    event_hub::TaskManager foreground_tasks;
    event_hub::TaskManager background_tasks;

    event_hub::RunLoop loop;
    loop.add(bus);
    loop.add(foreground_tasks);
    loop.add(background_tasks);

    endpoint.subscribe_queued<LogEvent>([&loop](const LogEvent& event) {
        std::cout << "event: " << event.text << '\n';
        if (event.text == "background timer fired") {
            loop.request_stop();
        }
    });

    foreground_tasks.post([&endpoint] {
        endpoint.post<LogEvent>("foreground task finished");
    });
    background_tasks.post_after(2ms, [&endpoint] {
        endpoint.post<LogEvent>("background timer fired");
    });

    loop.run();
}
