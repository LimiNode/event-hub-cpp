#include <event_hub.hpp>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

struct CommandEvent {
    std::string name;
};

int main() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint endpoint(bus);

    bus.set_exception_handler([](std::exception_ptr error) {
        try {
            if (error) {
                std::rethrow_exception(error);
            }
        } catch (const std::exception& ex) {
            std::cout << "handled exception: " << ex.what() << '\n';
        }
    });

    endpoint.subscribe_queued<CommandEvent>([](const CommandEvent& event) {
        if (event.name == "fail") {
            throw std::runtime_error("command failed");
        }
    });

    endpoint.subscribe_queued<CommandEvent>([](const CommandEvent& event) {
        std::cout << "observed command: " << event.name << '\n';
    });

    endpoint.post<CommandEvent>("fail");
    endpoint.post<CommandEvent>("ok");
    bus.process();

    event_hub::EventBus throwing_bus;
    event_hub::EventEndpoint throwing_endpoint(throwing_bus);

    throwing_endpoint.subscribe_queued<CommandEvent>([](const CommandEvent&) {
        throw std::runtime_error("unhandled command failed");
    });

    throwing_endpoint.post<CommandEvent>("fail");

    try {
        throwing_bus.process();
    } catch (const std::exception& ex) {
        std::cout << "rethrown exception: " << ex.what() << '\n';
    }
}
