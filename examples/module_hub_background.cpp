#include <event_hub.hpp>

#include <chrono>
#include <future>
#include <iostream>
#include <string>

using namespace std::chrono_literals;

struct CommandEvent {
    std::string text;
};

struct StatusEvent {
    std::string text;
};

class CommandModule final : public event_hub::Module {
public:
    explicit CommandModule(event_hub::EventBus& bus)
        : Module(bus) {}

private:
    void on_initialize() override {
        std::cout << "command module: initialize on hub thread\n";

        subscribe_queued<CommandEvent>([this](const CommandEvent& event) {
            tasks().post([this, text = event.text] {
                post<StatusEvent>("handled command: " + text);
            });

            if (event.text == "stop") {
                tasks().post_after(1ms, [this] {
                    post<StatusEvent>("stopping");
                });
            }
        });
    }

    void on_shutdown() noexcept override {
        std::cout << "command module: shutdown on hub thread\n";
    }
};

class WorkerSignalModule final : public event_hub::Module {
public:
    explicit WorkerSignalModule(event_hub::EventBus& bus)
        : Module(bus,
                 {event_hub::ModuleExecutionMode::private_thread, 8}) {}

private:
    void on_initialize() override {
        tasks().post_after(1ms, [this] {
            post<StatusEvent>("private worker is alive");
        });
    }
};

int main() {
    event_hub::ModuleHub hub;
    event_hub::EventEndpoint endpoint(hub.bus());
    std::promise<void> stopping_seen;
    auto stopping = stopping_seen.get_future();

    hub.emplace_module<CommandModule>();
    hub.emplace_module<WorkerSignalModule>();

    endpoint.subscribe_queued<StatusEvent>([&hub, &stopping_seen](const StatusEvent& event) {
        std::cout << "status: " << event.text << '\n';
        if (event.text == "stopping") {
            stopping_seen.set_value();
            hub.request_stop();
        }
    });

    hub.start();

    endpoint.post<CommandEvent>("refresh-cache");
    endpoint.post<CommandEvent>("stop");

    stopping.wait();
    hub.join();

    std::cout << "main: joined background hub thread\n";
}
