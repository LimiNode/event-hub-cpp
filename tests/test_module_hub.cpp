#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace event_hub_test;

namespace {

class LifecycleModule final : public event_hub::Module {
public:
    LifecycleModule(event_hub::EventBus& bus,
                    std::vector<std::string>& log,
                    std::string name)
        : Module(bus),
          m_log(&log),
          m_name(std::move(name)) {}

private:
    void on_initialize() override {
        m_log->push_back("init:" + m_name);
    }

    void on_shutdown() noexcept override {
        m_log->push_back("shutdown:" + m_name);
    }

    std::vector<std::string>* m_log = nullptr;
    std::string m_name;
};

class CountingModule final : public event_hub::Module {
public:
    CountingModule(event_hub::EventBus& bus,
                   event_hub::ModuleOptions options,
                   int& value)
        : Module(bus, options),
          m_value(&value) {}

    void add_task(int delta) {
        tasks().post([this, delta] {
            *m_value += delta;
        });
    }

private:
    int* m_value = nullptr;
};

class PostingModule final : public event_hub::Module {
public:
    explicit PostingModule(event_hub::EventBus& bus)
        : Module(bus) {}

    void publish_from_task(const std::string& text) {
        post<Message>(text);
    }

private:
    std::size_t on_process() override {
        if (m_posted_hook_event) {
            return 0;
        }

        m_posted_hook_event = true;
        post<Message>("hook");
        return 1;
    }

    bool m_posted_hook_event = false;
};

class DeadlineModule final : public event_hub::Module {
public:
    explicit DeadlineModule(event_hub::EventBus& bus)
        : Module(bus) {}

    void schedule(event_hub::Module::TimePoint due) {
        m_due = due;
    }

    int calls = 0;

private:
    std::size_t on_process() override {
        if (!m_due || *m_due > event_hub::TaskManager::Clock::now()) {
            return 0;
        }

        m_due = std::nullopt;
        ++calls;
        return 1;
    }

    std::optional<TimePoint> next_deadline_hint() const override {
        return m_due;
    }

    std::optional<TimePoint> m_due;
};

class PrivateProducerModule final : public event_hub::Module {
public:
    explicit PrivateProducerModule(event_hub::EventBus& bus)
        : Module(bus,
                 {event_hub::ModuleExecutionMode::private_thread, 16}) {}

    void publish_from_worker(const std::string& text) {
        post<Message>(text);
    }
};

class SelfShutdownModule final : public event_hub::Module {
public:
    SelfShutdownModule(event_hub::EventBus& bus,
                       std::promise<void>& stopped,
                       std::atomic<bool>& torn_down,
                       std::atomic<bool>& second_saw_teardown)
        : Module(bus,
                 {event_hub::ModuleExecutionMode::private_thread, 8}),
          m_stopped(&stopped),
          m_torn_down(&torn_down),
          m_second_saw_teardown(&second_saw_teardown) {}

    void schedule_self_shutdown() {
        tasks().post([this] {
            shutdown();
            m_stopped->set_value();
        });
        tasks().post([this] {
            m_second_saw_teardown->store(
                m_torn_down->load(std::memory_order_acquire),
                std::memory_order_release);
        });
    }

    void on_shutdown() noexcept override {
        m_torn_down->store(true, std::memory_order_release);
    }

private:
    std::promise<void>* m_stopped = nullptr;
    std::atomic<bool>* m_torn_down = nullptr;
    std::atomic<bool>* m_second_saw_teardown = nullptr;
};

class InitSignalModule final : public event_hub::Module {
public:
    InitSignalModule(event_hub::EventBus& bus,
                     std::promise<void>& initialized)
        : Module(bus),
          m_initialized(&initialized) {}

private:
    void on_initialize() override {
        m_initialized->set_value();
    }

    std::promise<void>* m_initialized = nullptr;
};

class StopOnProcessModule final : public event_hub::Module {
public:
    StopOnProcessModule(event_hub::EventBus& bus,
                        event_hub::ModuleHub& hub)
        : Module(bus),
          m_hub(&hub) {}

private:
    std::size_t on_process() override {
        if (!m_requested_stop) {
            m_requested_stop = true;
            m_hub->request_stop();
            return 1;
        }
        return 0;
    }

    event_hub::ModuleHub* m_hub = nullptr;
    bool m_requested_stop = false;
};

class ThrowingProcessModule final : public event_hub::Module {
public:
    explicit ThrowingProcessModule(event_hub::EventBus& bus)
        : Module(bus) {}

private:
    std::size_t on_process() override {
        throw std::runtime_error("module process failed");
    }
};

class ThrowingInitializeModule final : public event_hub::Module {
public:
    ThrowingInitializeModule(event_hub::EventBus& bus,
                             std::vector<std::string>& log)
        : Module(bus),
          m_log(&log) {}

private:
    void on_initialize() override {
        m_log->push_back("init:throw");
        throw std::runtime_error("module initialize failed");
    }

    void on_shutdown() noexcept override {
        m_log->push_back("shutdown:throw");
    }

    std::vector<std::string>* m_log = nullptr;
};

class ThrowingWorkerModule final : public event_hub::Module {
public:
    explicit ThrowingWorkerModule(event_hub::EventBus& bus)
        : Module(bus,
                 {event_hub::ModuleExecutionMode::private_thread, 8}) {}

    void fail_on_worker() {
        tasks().post([] {
            throw std::runtime_error("worker task failed");
        });
    }
};

bool wait_until(std::function<bool()> predicate,
                std::chrono::milliseconds timeout =
                    std::chrono::seconds(2)) {
    const auto deadline = event_hub::TaskManager::Clock::now() + timeout;
    while (event_hub::TaskManager::Clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

} // namespace

int main() {
    {
        event_hub::ModuleHub hub;
        std::vector<std::string> log;

        hub.emplace_module<LifecycleModule>(log, "first");
        hub.emplace_module<LifecycleModule>(log, "second");

        EVENT_HUB_TEST_CHECK(hub.module_count() == 2);

        hub.initialize();
        hub.shutdown();

        EVENT_HUB_TEST_CHECK((log == std::vector<std::string>{
                                         "init:first",
                                         "init:second",
                                         "shutdown:second",
                                         "shutdown:first"}));
    }

    {
        event_hub::ModuleHub hub;
        int first = 0;
        int second = 0;

        auto& first_module = hub.emplace_module<CountingModule>(
            event_hub::ModuleOptions{event_hub::ModuleExecutionMode::inline_in_hub,
                                     1},
            first);
        auto& second_module = hub.emplace_module<CountingModule>(
            event_hub::ModuleOptions{event_hub::ModuleExecutionMode::inline_in_hub,
                                     1},
            second);

        EVENT_HUB_TEST_CHECK(&first_module.tasks() != &second_module.tasks());

        first_module.add_task(1);
        first_module.add_task(10);
        second_module.add_task(100);
        second_module.add_task(1000);

        hub.initialize();

        EVENT_HUB_TEST_CHECK(hub.process() == 2);
        EVENT_HUB_TEST_CHECK(first == 1);
        EVENT_HUB_TEST_CHECK(second == 100);

        EVENT_HUB_TEST_CHECK(hub.process() == 2);
        EVENT_HUB_TEST_CHECK(first == 11);
        EVENT_HUB_TEST_CHECK(second == 1100);

        hub.shutdown();
    }

    {
        event_hub::ModuleHub hub;
        event_hub::EventEndpoint endpoint(hub.bus());
        std::vector<std::string> messages;

        auto& module = hub.emplace_module<PostingModule>();
        endpoint.subscribe_queued<Message>([&messages](const Message& message) {
            messages.push_back(message.text);
        });

        hub.initialize();
        module.tasks().post([&module] {
            module.publish_from_task("task");
        });

        EVENT_HUB_TEST_CHECK(hub.process() == 2);
        EVENT_HUB_TEST_CHECK(messages.empty());

        EVENT_HUB_TEST_CHECK(hub.process() == 2);
        EVENT_HUB_TEST_CHECK((messages == std::vector<std::string>{"task",
                                                                    "hook"}));

        hub.shutdown();
    }

    {
        event_hub::ModuleHub hub;
        CountingNotifier notifier;
        int value = 0;

        hub.set_notifier(&notifier);
        auto& module = hub.emplace_module<CountingModule>(
            event_hub::ModuleOptions{event_hub::ModuleExecutionMode::inline_in_hub,
                                     8},
            value);

        hub.initialize();

        const auto after_initialize =
            notifier.notifications.load(std::memory_order_relaxed);
        hub.bus().post<Message>("bus");
        EVENT_HUB_TEST_CHECK(notifier.notifications.load(
                                 std::memory_order_relaxed) >
                             after_initialize);

        const auto after_bus_post =
            notifier.notifications.load(std::memory_order_relaxed);
        module.tasks().post([&value] {
            value = 3;
        });
        EVENT_HUB_TEST_CHECK(notifier.notifications.load(
                                 std::memory_order_relaxed) >
                             after_bus_post);

        EVENT_HUB_TEST_CHECK(hub.process() == 2);
        EVENT_HUB_TEST_CHECK(value == 3);

        hub.reset_notifier();
        const auto after_reset =
            notifier.notifications.load(std::memory_order_relaxed);
        hub.bus().post<Message>("after reset");
        module.tasks().post([&value] {
            value = 5;
        });
        EVENT_HUB_TEST_CHECK(notifier.notifications.load(
                                 std::memory_order_relaxed) == after_reset);

        EVENT_HUB_TEST_CHECK(hub.process() == 2);
        EVENT_HUB_TEST_CHECK(value == 5);

        hub.shutdown();
    }

    {
        event_hub::ModuleHub hub;
        int manual_value = 0;
        auto& manual = hub.emplace_module<CountingModule>(
            event_hub::ModuleOptions{event_hub::ModuleExecutionMode::manual,
                                     1},
            manual_value);

        manual.tasks().post([&manual_value] {
            manual_value = 42;
        });

        hub.initialize();
        EVENT_HUB_TEST_CHECK(hub.process() == 0);
        EVENT_HUB_TEST_CHECK(manual_value == 0);
        EVENT_HUB_TEST_CHECK(manual.tasks().process() == 1);
        EVENT_HUB_TEST_CHECK(manual_value == 42);
        hub.shutdown();
    }

    {
        event_hub::ModuleHub hub;
        int value = 0;
        auto& delayed = hub.emplace_module<CountingModule>(
            event_hub::ModuleOptions{event_hub::ModuleExecutionMode::inline_in_hub,
                                     8},
            value);
        auto& hinted = hub.emplace_module<DeadlineModule>();

        hub.initialize();

        delayed.tasks().post_after(std::chrono::milliseconds(25), [&value] {
            value = 7;
        });
        hinted.schedule(event_hub::TaskManager::Clock::now() +
                        std::chrono::milliseconds(25));

        EVENT_HUB_TEST_CHECK(!hub.has_pending());
        EVENT_HUB_TEST_CHECK(hub.next_deadline().has_value());

        std::this_thread::sleep_for(std::chrono::milliseconds(40));

        EVENT_HUB_TEST_CHECK(hub.has_pending());
        EVENT_HUB_TEST_CHECK(hub.process() == 2);
        EVENT_HUB_TEST_CHECK(value == 7);
        EVENT_HUB_TEST_CHECK(hinted.calls == 1);

        hub.shutdown();
    }

    {
        event_hub::ModuleHub hub;
        event_hub::EventEndpoint endpoint(hub.bus());
        std::vector<std::string> messages;
        auto& producer = hub.emplace_module<PrivateProducerModule>();

        endpoint.subscribe_queued<Message>([&messages](const Message& message) {
            messages.push_back(message.text);
        });

        hub.initialize();
        producer.tasks().post([&producer] {
            producer.publish_from_worker("private");
        });

        EVENT_HUB_TEST_CHECK(wait_until([&hub] {
            return hub.bus().has_pending();
        }));

        EVENT_HUB_TEST_CHECK(hub.process() == 1);
        EVENT_HUB_TEST_CHECK((messages == std::vector<std::string>{"private"}));

        hub.shutdown();
    }

    {
        event_hub::ModuleHub hub;
        auto& worker = hub.emplace_module<ThrowingWorkerModule>();

        hub.initialize();
        worker.fail_on_worker();

        bool threw = false;
        EVENT_HUB_TEST_CHECK(wait_until([&hub, &threw] {
            try {
                (void)hub.process();
            } catch (const std::runtime_error&) {
                threw = true;
                return true;
            }

            return false;
        }));
        EVENT_HUB_TEST_CHECK(threw);

        hub.shutdown();
        EVENT_HUB_TEST_CHECK(worker.is_stopped());
    }

    {
        event_hub::ModuleHub hub;
        std::promise<void> initialized;
        auto initialized_future = initialized.get_future();
        auto& module = hub.emplace_module<InitSignalModule>(initialized);

        hub.start();
        require_ready(initialized_future);

        module.tasks().post([&hub] {
            hub.request_stop();
        });

        hub.join();
        EVENT_HUB_TEST_CHECK(module.is_stopped());
    }

    {
        event_hub::ModuleHub hub;
        auto& module = hub.emplace_module<StopOnProcessModule>(hub);

        hub.run();

        EVENT_HUB_TEST_CHECK(module.is_stopped());
        EVENT_HUB_TEST_CHECK(hub.stop_requested());
    }

    {
        event_hub::ModuleHub hub;
        std::vector<std::string> log;
        auto& tracked = hub.emplace_module<LifecycleModule>(log, "tracked");
        hub.emplace_module<ThrowingProcessModule>();

        hub.start();

        bool threw = false;
        try {
            hub.join();
        } catch (const std::runtime_error&) {
            threw = true;
        }

        EVENT_HUB_TEST_CHECK(threw);
        EVENT_HUB_TEST_CHECK(tracked.is_stopped());
        EVENT_HUB_TEST_CHECK((log == std::vector<std::string>{
                                         "init:tracked",
                                         "shutdown:tracked"}));
    }

    {
        event_hub::ModuleHub hub;
        std::vector<std::string> log;
        auto& initialized =
            hub.emplace_module<LifecycleModule>(log, "initialized");
        auto& throwing = hub.emplace_module<ThrowingInitializeModule>(log);

        bool threw = false;
        try {
            hub.initialize();
        } catch (const std::runtime_error&) {
            threw = true;
        }

        EVENT_HUB_TEST_CHECK(threw);
        EVENT_HUB_TEST_CHECK(initialized.is_stopped());
        EVENT_HUB_TEST_CHECK(throwing.is_stopped());
        EVENT_HUB_TEST_CHECK((log == std::vector<std::string>{
                                         "init:initialized",
                                         "init:throw",
                                         "shutdown:initialized"}));
    }

    {
        event_hub::ModuleHub hub;
        int value = 0;

        bool null_module_rejected = false;
        try {
            hub.add_module(nullptr);
        } catch (const std::invalid_argument&) {
            null_module_rejected = true;
        }
        EVENT_HUB_TEST_CHECK(null_module_rejected);

        event_hub::EventBus foreign_bus;
        auto foreign_module = std::make_unique<CountingModule>(
            foreign_bus,
            event_hub::ModuleOptions{},
            value);

        bool foreign_bus_rejected = false;
        try {
            hub.add_module(std::move(foreign_module));
        } catch (const std::invalid_argument&) {
            foreign_bus_rejected = true;
        }
        EVENT_HUB_TEST_CHECK(foreign_bus_rejected);

        hub.emplace_module<CountingModule>(event_hub::ModuleOptions{}, value);
        hub.initialize();

        bool add_after_initialize_rejected = false;
        try {
            hub.emplace_module<CountingModule>(event_hub::ModuleOptions{},
                                               value);
        } catch (const std::logic_error&) {
            add_after_initialize_rejected = true;
        }
        EVENT_HUB_TEST_CHECK(add_after_initialize_rejected);

        hub.shutdown();
    }

    {
        event_hub::ModuleHub hub;

        hub.start();

        bool second_start_rejected = false;
        try {
            hub.start();
        } catch (const std::logic_error&) {
            second_start_rejected = true;
        }
        EVENT_HUB_TEST_CHECK(second_start_rejected);

        hub.request_stop();
        hub.join();
    }

    {
        // shutdown() may be called by the private worker itself. Joining is
        // deferred until the owning thread calls shutdown() again.
        std::promise<void> stopped;
        auto stopped_future = stopped.get_future();
        event_hub::ModuleHub hub;
        std::atomic<bool> torn_down{false};
        std::atomic<bool> second_saw_teardown{false};
        auto& module = hub.emplace_module<SelfShutdownModule>(
            stopped, torn_down, second_saw_teardown);
        hub.initialize();
        module.schedule_self_shutdown();
        EVENT_HUB_TEST_CHECK(stopped_future.wait_for(std::chrono::seconds(2)) ==
                             std::future_status::ready);
        hub.shutdown();
        EVENT_HUB_TEST_CHECK(module.is_stopped());
        EVENT_HUB_TEST_CHECK(!second_saw_teardown.load());
    }

    return 0;
}
