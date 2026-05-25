#include <event_hub.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <utility>

using namespace std::chrono_literals;

struct LogEvent {
    std::string source;
    std::string text;
};

class ConsoleModule final : public event_hub::Module {
public:
    explicit ConsoleModule(event_hub::EventBus& bus)
        : Module(bus) {}

private:
    void on_initialize() override {
        std::cout << "console: initialize\n";
        subscribe_queued<LogEvent>([](const LogEvent& event) {
            std::cout << event.source << ": " << event.text << '\n';
        });
    }

    void on_shutdown() noexcept override {
        std::cout << "console: shutdown\n";
    }
};

class InlineSchedulerModule final : public event_hub::Module {
public:
    explicit InlineSchedulerModule(event_hub::EventBus& bus)
        : Module(bus,
                 {event_hub::ModuleExecutionMode::inline_in_hub, 4}) {}

private:
    void on_initialize() override {
        std::cout << "inline scheduler: initialize\n";

        tasks().post([this] {
            post<LogEvent>("inline scheduler", "immediate task");
        });

        tasks().post_after(2ms, [this] {
            post<LogEvent>("inline scheduler", "delayed task");
        });
    }

    void on_shutdown() noexcept override {
        std::cout << "inline scheduler: shutdown\n";
    }
};

class WorkerModule final : public event_hub::Module {
public:
    explicit WorkerModule(event_hub::EventBus& bus)
        : Module(bus,
                 {event_hub::ModuleExecutionMode::private_thread, 8}) {}

private:
    void on_initialize() override {
        std::cout << "worker: initialize\n";

        tasks().post([this] {
            post<LogEvent>("worker", "task on private module thread");
        });

        tasks().post_after(4ms, [this] {
            post<LogEvent>("worker", "finished");
        });
    }

    void on_shutdown() noexcept override {
        std::cout << "worker: shutdown\n";
    }
};

int main() {
    event_hub::ModuleHub hub;
    event_hub::EventEndpoint control(hub.bus());

    hub.emplace_module<ConsoleModule>();
    hub.emplace_module<InlineSchedulerModule>();
    hub.emplace_module<WorkerModule>();

    control.subscribe_queued<LogEvent>([&hub](const LogEvent& event) {
        if (event.source == "worker" && event.text == "finished") {
            hub.request_stop();
        }
    });

    std::cout << "hub: run() initializes, blocks, then shuts down\n";
    hub.run();
    std::cout << "hub: stopped\n";
}
