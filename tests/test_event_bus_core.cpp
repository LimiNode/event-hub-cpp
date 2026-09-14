#include "test_helpers.hpp"

#include <atomic>
#include <thread>
#include <typeindex>
#include <vector>

using namespace event_hub_test;

int main() {
    {
        event_hub::EventBus bus;
        int sync_total = 0;

        {
            event_hub::EventEndpoint endpoint(bus);
            endpoint.subscribe_direct<Ping>([&sync_total](const Ping& ping) {
                sync_total += ping.value;
            });

            const auto result = endpoint.emit_direct<Ping>(2);
            EVENT_HUB_TEST_CHECK(result.matched == 1);
            EVENT_HUB_TEST_CHECK(result.delivered == 1);
            EVENT_HUB_TEST_CHECK(result.skipped == 0);
            EVENT_HUB_TEST_CHECK(sync_total == 2);
        }

        const auto result = bus.emit_direct<Ping>(3);
        EVENT_HUB_TEST_CHECK(result.matched == 0);
        EVENT_HUB_TEST_CHECK(result.delivered == 0);
        EVENT_HUB_TEST_CHECK(result.skipped == 0);
        EVENT_HUB_TEST_CHECK(sync_total == 2);
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        int direct_total = 0;
        int queued_total = 0;
        int any_total = 0;
        std::vector<event_hub::DeliveryMismatch> mismatches;

        bus.set_delivery_mismatch_handler(
            [&mismatches](const event_hub::DeliveryMismatch& mismatch) {
                mismatches.push_back(mismatch);
            });

        endpoint.subscribe_direct<Ping>([&direct_total](const Ping& ping) {
            direct_total += ping.value;
        });
        endpoint.subscribe_queued<Ping>([&queued_total](const Ping& ping) {
            queued_total += ping.value;
        });
        endpoint.subscribe_any<Ping>([&any_total](const Ping& ping) {
            any_total += ping.value;
        });

        const auto direct_result = endpoint.emit_direct<Ping>(2);
        EVENT_HUB_TEST_CHECK(direct_result.matched == 3);
        EVENT_HUB_TEST_CHECK(direct_result.delivered == 2);
        EVENT_HUB_TEST_CHECK(direct_result.skipped == 1);
        EVENT_HUB_TEST_CHECK(direct_total == 2);
        EVENT_HUB_TEST_CHECK(queued_total == 0);
        EVENT_HUB_TEST_CHECK(any_total == 2);
        EVENT_HUB_TEST_CHECK(mismatches.empty());

        endpoint.post<Ping>(3);
        EVENT_HUB_TEST_CHECK(bus.process() == 1);
        EVENT_HUB_TEST_CHECK(direct_total == 2);
        EVENT_HUB_TEST_CHECK(queued_total == 3);
        EVENT_HUB_TEST_CHECK(any_total == 5);
        EVENT_HUB_TEST_CHECK(mismatches.empty());
    }

    {
        // An already-constructed non-const lvalue must select the copying
        // overload instead of attempting to instantiate make_shared<E&>.
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        int received = 0;
        endpoint.subscribe_queued<Ping>([&received](const Ping& ping) {
            received = ping.value;
        });

        Ping event{17};
        bus.post(event);
        event.value = 99;

        EVENT_HUB_TEST_CHECK(bus.process() == 1);
        EVENT_HUB_TEST_CHECK(received == 17);

        Ping endpoint_event{23};
        endpoint.post(endpoint_event);
        endpoint_event.value = 101;

        EVENT_HUB_TEST_CHECK(bus.process() == 1);
        EVENT_HUB_TEST_CHECK(received == 23);
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        std::vector<event_hub::DeliveryMismatch> mismatches;

        bus.set_delivery_mismatch_handler(
            [&mismatches](const event_hub::DeliveryMismatch& mismatch) {
                mismatches.push_back(mismatch);
            });

        endpoint.subscribe_queued<Ping>([](const Ping&) {
            EVENT_HUB_TEST_CHECK(false && "queued subscriber must be skipped");
        });

        const auto direct_result = endpoint.emit_direct<Ping>(1);
        EVENT_HUB_TEST_CHECK(direct_result.matched == 1);
        EVENT_HUB_TEST_CHECK(direct_result.delivered == 0);
        EVENT_HUB_TEST_CHECK(direct_result.skipped == 1);
        EVENT_HUB_TEST_CHECK(mismatches.size() == 1);
        EVENT_HUB_TEST_CHECK(mismatches[0].event_type ==
                             std::type_index(typeid(Ping)));
        EVENT_HUB_TEST_CHECK(mismatches[0].dispatch_policy ==
                             event_hub::DeliveryPolicy::direct);
        EVENT_HUB_TEST_CHECK(mismatches[0].skipped_subscribers == 1);

        endpoint.unsubscribe_all();
        endpoint.subscribe_direct<Ping>([](const Ping&) {
            EVENT_HUB_TEST_CHECK(false && "direct subscriber must be skipped");
        });

        endpoint.post<Ping>(2);
        EVENT_HUB_TEST_CHECK(bus.process() == 1);
        EVENT_HUB_TEST_CHECK(mismatches.size() == 2);
        EVENT_HUB_TEST_CHECK(mismatches[1].event_type ==
                             std::type_index(typeid(Ping)));
        EVENT_HUB_TEST_CHECK(mismatches[1].dispatch_policy ==
                             event_hub::DeliveryPolicy::queued);
        EVENT_HUB_TEST_CHECK(mismatches[1].skipped_subscribers == 1);
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        int direct_total = 0;
        std::vector<event_hub::DeliveryMismatch> mismatches;

        bus.set_delivery_mismatch_handler(
            [&mismatches](const event_hub::DeliveryMismatch& mismatch) {
                mismatches.push_back(mismatch);
            },
            event_hub::DeliveryMismatchReportMode::any_skipped);

        endpoint.subscribe_direct<Ping>([&direct_total](const Ping& ping) {
            direct_total += ping.value;
        });
        endpoint.subscribe_queued<Ping>([](const Ping&) {
            EVENT_HUB_TEST_CHECK(false && "queued subscriber must be skipped");
        });

        const auto direct_result = endpoint.emit_direct<Ping>(4);
        EVENT_HUB_TEST_CHECK(direct_result.matched == 2);
        EVENT_HUB_TEST_CHECK(direct_result.delivered == 1);
        EVENT_HUB_TEST_CHECK(direct_result.skipped == 1);
        EVENT_HUB_TEST_CHECK(direct_total == 4);
        EVENT_HUB_TEST_CHECK(mismatches.size() == 1);
        EVENT_HUB_TEST_CHECK(mismatches[0].dispatch_policy ==
                             event_hub::DeliveryPolicy::direct);
        EVENT_HUB_TEST_CHECK(mismatches[0].skipped_subscribers == 1);
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        int ping_calls = 0;
        int message_calls = 0;

        endpoint.subscribe_direct<Ping>([&ping_calls](const Ping&) {
            ++ping_calls;
        });
        endpoint.subscribe_direct<Message>([&message_calls](const Message&) {
            ++message_calls;
        });

        endpoint.unsubscribe<Ping>();
        endpoint.emit_direct<Ping>(1);
        endpoint.emit_direct<Message>("still subscribed");

        EVENT_HUB_TEST_CHECK(ping_calls == 0);
        EVENT_HUB_TEST_CHECK(message_calls == 1);
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        int calls = 0;

        const auto id = endpoint.subscribe_direct<Ping>([&calls](const Ping&) {
            ++calls;
        });

        endpoint.unsubscribe(id);
        endpoint.emit_direct<Ping>(1);
        EVENT_HUB_TEST_CHECK(calls == 0);

        endpoint.subscribe_direct<Ping>([&calls](const Ping&) {
            ++calls;
        });
        endpoint.subscribe_direct<Message>([&calls](const Message&) {
            ++calls;
        });

        endpoint.unsubscribe_all();
        endpoint.emit_direct<Ping>(1);
        endpoint.emit_direct<Message>("ignored");
        EVENT_HUB_TEST_CHECK(calls == 0);
    }

    {
        event_hub::EventBus bus;
        int total = 0;
        int owner = 0;

        const auto id = bus.subscribe<Ping>(
            &owner,
            event_hub::DeliveryPolicy::direct,
            [&total](const Ping& ping) {
                total += ping.value;
            });

        bus.emit_direct<Ping>(3);
        EVENT_HUB_TEST_CHECK(total == 3);

        bus.unsubscribe(id);
        bus.emit_direct<Ping>(4);
        EVENT_HUB_TEST_CHECK(total == 3);

        bus.subscribe<Ping>(
            &owner,
            event_hub::DeliveryPolicy::direct,
            [&total](const Ping& ping) {
                total += ping.value;
            });
        bus.unsubscribe_all(&owner);
        bus.emit_direct<Ping>(5);
        EVENT_HUB_TEST_CHECK(total == 3);
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        int move_only_total = 0;

        endpoint.subscribe_direct<MoveOnly>(
            [&move_only_total](const MoveOnly& event) {
                move_only_total += *event.value;
            });

        MoveOnly event(11);
        endpoint.emit_direct<MoveOnly>(event);
        EVENT_HUB_TEST_CHECK(move_only_total == 11);
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        int async_total = 0;

        endpoint.subscribe_queued<Ping>([&async_total](const Ping& ping) {
            async_total += ping.value;
        });

        endpoint.post<Ping>(4);
        endpoint.post<Ping>(6);

        EVENT_HUB_TEST_CHECK(bus.pending_count() == 2);
        EVENT_HUB_TEST_CHECK(bus.process() == 2);
        EVENT_HUB_TEST_CHECK(async_total == 10);
        EVENT_HUB_TEST_CHECK(!bus.has_pending());
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        std::vector<std::string> messages;

        endpoint.subscribe_queued<Message>([&messages](const Message& message) {
            messages.push_back(message.text);
        });

        const Message copied{"copy"};
        endpoint.post<Message>(copied);

        Message moved{"move"};
        endpoint.post<Message>(std::move(moved));

        endpoint.post<Message>("aggregate");

        EVENT_HUB_TEST_CHECK(bus.process() == 3);
        EVENT_HUB_TEST_CHECK((messages == std::vector<std::string>{"copy", "move", "aggregate"}));
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        std::vector<int> seen;

        endpoint.subscribe_queued<Ping>([&endpoint, &seen](const Ping& ping) {
            seen.push_back(ping.value);
            if (ping.value == 1) {
                endpoint.post<Ping>(2);
            }
        });

        endpoint.post<Ping>(1);

        EVENT_HUB_TEST_CHECK(bus.process() == 1);
        EVENT_HUB_TEST_CHECK((seen == std::vector<int>{1}));
        EVENT_HUB_TEST_CHECK(bus.pending_count() == 1);

        EVENT_HUB_TEST_CHECK(bus.process() == 1);
        EVENT_HUB_TEST_CHECK((seen == std::vector<int>{1, 2}));
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        int calls = 0;

        endpoint.subscribe_queued<Ping>([&calls](const Ping&) {
            ++calls;
        });

        endpoint.post<Ping>(1);
        endpoint.post<Ping>(2);
        EVENT_HUB_TEST_CHECK(bus.pending_count() == 2);

        bus.clear_pending();
        EVENT_HUB_TEST_CHECK(!bus.has_pending());
        EVENT_HUB_TEST_CHECK(bus.process() == 0);
        EVENT_HUB_TEST_CHECK(calls == 0);

        endpoint.post<Ping>(3);
        bus.clear();
        EVENT_HUB_TEST_CHECK(!bus.has_pending());
        EVENT_HUB_TEST_CHECK(bus.process() == 0);
        EVENT_HUB_TEST_CHECK(calls == 0);
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        std::atomic_int total{0};

        endpoint.subscribe_queued<Ping>([&total](const Ping& ping) {
            total.fetch_add(ping.value, std::memory_order_relaxed);
        });

        std::thread producer([&endpoint] {
            endpoint.post<Ping>(7);
        });

        producer.join();
        EVENT_HUB_TEST_CHECK(bus.process() == 1);
        EVENT_HUB_TEST_CHECK(total.load(std::memory_order_relaxed) == 7);
    }

    return 0;
}
