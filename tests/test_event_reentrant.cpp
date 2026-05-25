#include "test_helpers.hpp"

#include <vector>

using namespace event_hub_test;

int main() {
    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        std::vector<int> calls;
        event_hub::EventBus::SubscriptionId self_id = 0;

        self_id = endpoint.subscribe_direct<Ping>([&](const Ping&) {
            calls.push_back(1);
            endpoint.unsubscribe(self_id);
        });

        endpoint.emit<Ping>(1);
        endpoint.emit<Ping>(1);

        EVENT_HUB_TEST_CHECK((calls == std::vector<int>{1}));
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        std::vector<int> calls;
        event_hub::EventBus::SubscriptionId second_id = 0;

        endpoint.subscribe_direct<Ping>([&](const Ping&) {
            calls.push_back(1);
            endpoint.unsubscribe(second_id);
        });
        second_id = endpoint.subscribe_direct<Ping>([&](const Ping&) {
            calls.push_back(2);
        });

        endpoint.emit<Ping>(1);
        endpoint.emit<Ping>(1);

        EVENT_HUB_TEST_CHECK((calls == std::vector<int>{1, 2, 1}));
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        std::vector<int> calls;
        bool subscribed_extra = false;

        endpoint.subscribe_direct<Ping>([&](const Ping&) {
            calls.push_back(1);
            if (!subscribed_extra) {
                subscribed_extra = true;
                endpoint.subscribe_direct<Ping>([&](const Ping&) {
                    calls.push_back(2);
                });
            }
        });

        endpoint.emit<Ping>(1);
        endpoint.emit<Ping>(1);

        EVENT_HUB_TEST_CHECK((calls == std::vector<int>{1, 1, 2}));
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        std::vector<int> calls;
        event_hub::EventBus::SubscriptionId message_id = 0;

        endpoint.subscribe_direct<Ping>([&](const Ping&) {
            calls.push_back(1);
            endpoint.unsubscribe(message_id);
        });
        message_id = endpoint.subscribe_direct<Message>([&](const Message&) {
            calls.push_back(2);
        });

        endpoint.emit<Ping>(1);
        endpoint.emit<Message>("ignored");

        EVENT_HUB_TEST_CHECK((calls == std::vector<int>{1}));
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        std::vector<int> calls;

        endpoint.subscribe_direct<Ping>([&](const Ping&) {
            calls.push_back(1);
            endpoint.unsubscribe<Ping>();
        });
        endpoint.subscribe_direct<Ping>([&](const Ping&) {
            calls.push_back(2);
        });
        endpoint.subscribe_direct<Message>([&](const Message&) {
            calls.push_back(3);
        });

        endpoint.emit<Ping>(1);
        endpoint.emit<Ping>(1);
        endpoint.emit<Message>("still active");

        EVENT_HUB_TEST_CHECK((calls == std::vector<int>{1, 2, 3}));
    }

    return 0;
}
