#include "test_helpers.hpp"

#include <chrono>

using namespace event_hub_test;

int main() {
    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        CountingNotifier notifier;
        int total = 0;

        endpoint.subscribe_any<Ping>([&total](const Ping& ping) {
            total += ping.value;
        });

        bus.set_notifier(&notifier);

        endpoint.emit<Ping>(1);
        EVENT_HUB_TEST_CHECK(notifier.notifications.load(std::memory_order_relaxed) == 0);
        EVENT_HUB_TEST_CHECK(total == 1);

        endpoint.post<Ping>(2);
        endpoint.post<Ping>(3);
        endpoint.post<Ping>(4);
        EVENT_HUB_TEST_CHECK(notifier.notifications.load(std::memory_order_relaxed) == 3);

        bus.reset_notifier();
        endpoint.post<Ping>(5);
        EVENT_HUB_TEST_CHECK(notifier.notifications.load(std::memory_order_relaxed) == 3);

        EVENT_HUB_TEST_CHECK(bus.pending_count() == 4);
        EVENT_HUB_TEST_CHECK(bus.process() == 4);
        EVENT_HUB_TEST_CHECK(total == 15);
    }

    {
        event_hub::SyncNotifier notifier;
        const auto generation = notifier.generation();

        notifier.notify();

        EVENT_HUB_TEST_CHECK(notifier.generation() == generation + 1);
        EVENT_HUB_TEST_CHECK(notifier.wait_for_ms(generation, 0));
        EVENT_HUB_TEST_CHECK(!notifier.wait_for(notifier.generation(),
                                  std::chrono::milliseconds(0)));
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        event_hub::SyncNotifier notifier;
        int total = 0;

        endpoint.subscribe_queued<Ping>([&total](const Ping& ping) {
            total += ping.value;
        });
        bus.set_notifier(&notifier);

        const auto generation = notifier.generation();
        const bool did_work = bus.process() > 0;
        EVENT_HUB_TEST_CHECK(!did_work);

        endpoint.post<Ping>(6);
        EVENT_HUB_TEST_CHECK(notifier.wait_for_ms(generation, 0));
        EVENT_HUB_TEST_CHECK(bus.process() == 1);
        EVENT_HUB_TEST_CHECK(total == 6);
    }

    return 0;
}
