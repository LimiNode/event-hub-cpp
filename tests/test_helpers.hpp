#pragma once

#include <event_hub.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace event_hub_test {

[[noreturn]] inline void fail_check(const char* expression,
                                    const char* file,
                                    int line) {
    std::cerr << file << ':' << line << ": check failed: " << expression
              << std::endl;
    std::abort();
}

inline void check(bool condition,
                  const char* expression,
                  const char* file,
                  int line) {
    if (!condition) {
        fail_check(expression, file, line);
    }
}

template <typename Future>
void require_ready(Future& future,
                   std::chrono::milliseconds timeout =
                       std::chrono::seconds(2)) {
    check(future.wait_for(timeout) == std::future_status::ready,
          "future.wait_for(timeout) == std::future_status::ready",
          __FILE__,
          __LINE__);
}

template <typename Predicate>
void require_until(Predicate&& predicate,
                   std::chrono::milliseconds timeout =
                       std::chrono::seconds(2)) {
    const auto deadline = event_hub::TaskManager::Clock::now() + timeout;
    while (event_hub::TaskManager::Clock::now() < deadline) {
        if (predicate()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    check(predicate(), "predicate()", __FILE__, __LINE__);
}

inline void wait_until_processed(event_hub::TaskManager& tasks,
                                 std::size_t expected_count,
                                 std::chrono::milliseconds timeout =
                                     std::chrono::seconds(2)) {
    std::size_t processed = 0;
    require_until(
        [&] {
            processed += tasks.process();
            return processed >= expected_count;
        },
        timeout);
    check(processed == expected_count,
          "processed == expected_count",
          __FILE__,
          __LINE__);
}

struct Ping {
    int value = 0;
};

struct Message {
    std::string text;
};

class CountingNotifier final : public event_hub::INotifier {
public:
    void notify() noexcept override {
        notifications.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic_int notifications{0};
};

struct MoveOnly {
    explicit MoveOnly(int value_)
        : value(std::make_unique<int>(value_)) {}

    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) noexcept = default;
    MoveOnly& operator=(MoveOnly&&) noexcept = default;

    std::unique_ptr<int> value;
};

struct LifetimeProbe {
    explicit LifetimeProbe(std::atomic_bool& destroyed_)
        : destroyed(&destroyed_) {}

    ~LifetimeProbe() {
        destroyed->store(true, std::memory_order_relaxed);
    }

    std::atomic_bool* destroyed = nullptr;
};

class DerivedEvent final : public event_hub::Event {
public:
    explicit DerivedEvent(int value_ = 0)
        : value(value_) {}

    EVENT_HUB_EVENT(DerivedEvent)

    int value = 0;
};

class OtherEvent final : public event_hub::Event {
public:
    EVENT_HUB_EVENT(OtherEvent)
};

class CountingListener final : public event_hub::EventListener {
public:
    void on_event(const event_hub::Event& event) override {
        const auto& derived = event.as_ref<DerivedEvent>();
        total += derived.value;
    }

    int total = 0;
};

struct NodeTestEvent final : event_hub::Event {
    explicit NodeTestEvent(int value_)
        : value(value_) {}

    EVENT_HUB_EVENT(NodeTestEvent)

    int value = 0;
};

class TestNode final : public event_hub::EventNode {
public:
    explicit TestNode(event_hub::EventBus& bus)
        : EventNode(bus) {}

    void start() {
        listen_queued<NodeTestEvent>();
    }

    void on_event(const event_hub::Event& event) override {
        if (const auto* ev = event.as<NodeTestEvent>()) {
            received += ev->value;
        }
    }

    int received = 0;
};

class StrictLifetimeModule final
    : public std::enable_shared_from_this<StrictLifetimeModule> {
public:
    StrictLifetimeModule(event_hub::EventBus& bus,
                         std::atomic_bool& destroyed,
                         std::atomic_bool& handled)
        : m_endpoint(bus),
          m_destroyed(&destroyed),
          m_handled(&handled) {}

    ~StrictLifetimeModule() {
        m_destroyed->store(true, std::memory_order_relaxed);
    }

    void start(std::promise<void>& entered,
               std::shared_future<void> release) {
        auto weak = weak_from_this();

        m_endpoint.subscribe_direct<Ping>(
            weak,
            [weak, &entered, release](const Ping&) {
                auto self = weak.lock();
                if (!self) {
                    return;
                }

                entered.set_value();
                release.wait();
                check(!self->is_destroyed(),
                      "!self->is_destroyed()",
                      __FILE__,
                      __LINE__);
                self->handle();
            });
    }

private:
    bool is_destroyed() const {
        return m_destroyed->load(std::memory_order_relaxed);
    }

    void handle() {
        m_handled->store(true, std::memory_order_relaxed);
    }

    event_hub::EventEndpoint m_endpoint;
    std::atomic_bool* m_destroyed = nullptr;
    std::atomic_bool* m_handled = nullptr;
};

} // namespace event_hub_test

#define EVENT_HUB_TEST_CHECK(condition)                                      \
    ::event_hub_test::check(static_cast<bool>(condition),                    \
                            #condition,                                      \
                            __FILE__,                                        \
                            __LINE__)
