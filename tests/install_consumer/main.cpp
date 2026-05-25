#include <event_hub.hpp>

#include <string>

struct ConsumerEvent {
    std::string message;
};

int main() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint endpoint(bus);

    int received = 0;
    endpoint.subscribe_queued<ConsumerEvent>([&received](const ConsumerEvent& event) {
        if (event.message == "installed") {
            ++received;
        }
    });

    endpoint.post<ConsumerEvent>("installed");

    return bus.process() == 1 && received == 1 ? 0 : 1;
}
