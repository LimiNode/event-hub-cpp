#include "test_helpers.hpp"

#include <stdexcept>
#include <vector>

using namespace event_hub_test;

int main() {
    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        int callback_total = 0;

        endpoint.subscribe_direct<Ping>([](const Ping&) {
            throw std::runtime_error("callback failed");
        });
        endpoint.subscribe_direct<Ping>([&callback_total](const Ping& ping) {
            callback_total += ping.value;
        });

        bool threw = false;
        try {
            endpoint.emit<Ping>(1);
        } catch (const std::runtime_error&) {
            threw = true;
        }

        EVENT_HUB_TEST_CHECK(threw);
        EVENT_HUB_TEST_CHECK(callback_total == 0);
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);

        int exception_count = 0;
        int callback_total = 0;
        bus.set_exception_handler([&exception_count](std::exception_ptr exception) {
            try {
                if (exception) {
                    std::rethrow_exception(exception);
                }
            } catch (const std::runtime_error&) {
                ++exception_count;
            }
        });

        endpoint.subscribe_queued<Ping>([](const Ping&) {
            throw std::runtime_error("callback failed");
        });
        endpoint.subscribe_queued<Ping>([&callback_total](const Ping& ping) {
            callback_total += ping.value;
        });

        endpoint.post<Ping>(1);
        endpoint.post<Ping>(2);

        EVENT_HUB_TEST_CHECK(bus.process() == 2);
        EVENT_HUB_TEST_CHECK(exception_count == 2);
        EVENT_HUB_TEST_CHECK(callback_total == 3);
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        std::vector<int> seen;

        endpoint.subscribe_queued<Ping>([&bus, &seen](const Ping& ping) {
            if (ping.value == 1) {
                bus.post<Ping>(4);
                throw std::runtime_error("callback failed");
            }
            seen.push_back(ping.value);
        });

        endpoint.post<Ping>(1);
        endpoint.post<Ping>(2);
        endpoint.post<Ping>(3);

        bool threw = false;
        try {
            (void)bus.process();
        } catch (const std::runtime_error&) {
            threw = true;
        }

        EVENT_HUB_TEST_CHECK(threw);
        EVENT_HUB_TEST_CHECK(bus.pending_count() == 3);

        EVENT_HUB_TEST_CHECK(bus.process() == 3);
        EVENT_HUB_TEST_CHECK((seen == std::vector<int>{2, 3, 4}));
    }

    return 0;
}
