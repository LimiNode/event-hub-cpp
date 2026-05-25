#include <event_hub.hpp>

#include <iostream>
#include <string>

struct UserLoggedInEvent {
    std::string name;
};

struct TaskQueuedEvent {
    std::string task_id;
};

int main() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint endpoint(bus);

    endpoint.subscribe_direct<UserLoggedInEvent>([](const UserLoggedInEvent& event) {
        std::cout << "user logged in: " << event.name << '\n';
    });

    const auto task_subscription = endpoint.subscribe_queued<TaskQueuedEvent>(
        [](const TaskQueuedEvent& event) {
            std::cout << "task queued: " << event.task_id << '\n';
        });

    endpoint.emit_direct<UserLoggedInEvent>("alice");

    endpoint.post<TaskQueuedEvent>("build-docs");
    std::cout << "pending before process: " << bus.pending_count() << '\n';
    bus.process();

    endpoint.unsubscribe(task_subscription);
    endpoint.post<TaskQueuedEvent>("will-not-print");
    bus.process();

    endpoint.post<TaskQueuedEvent>("will-be-cleared");
    std::cout << "pending before clear: " << bus.pending_count() << '\n';
    bus.clear_pending();
    std::cout << "pending after clear: " << bus.pending_count() << '\n';
}
