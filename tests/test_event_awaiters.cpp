#include "test_helpers.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

using namespace event_hub_test;

int main() {
    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        int awaited = 0;

        endpoint.await_once<Ping>(
            [](const Ping& ping) { return ping.value == 8; },
            [&awaited](const Ping& ping) { awaited += ping.value; });

        endpoint.post<Ping>(7);
        endpoint.post<Ping>(8);
        endpoint.post<Ping>(9);

        EVENT_HUB_TEST_CHECK(bus.process() == 3);
        EVENT_HUB_TEST_CHECK(awaited == 8);

        endpoint.emit_direct<Ping>(8);
        EVENT_HUB_TEST_CHECK(awaited == 8);
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        int even_total = 0;
        event_hub::AwaitOptions direct_options;
        direct_options.set_delivery(event_hub::DeliveryPolicy::direct);

        auto awaiter = endpoint.await_each<Ping>(
            [](const Ping& ping) { return ping.value % 2 == 0; },
            [&even_total](const Ping& ping) {
                even_total += ping.value;
            },
            direct_options);

        endpoint.emit_direct<Ping>(1);
        endpoint.emit_direct<Ping>(2);
        endpoint.emit_direct<Ping>(4);
        awaiter->cancel();
        endpoint.emit_direct<Ping>(6);

        EVENT_HUB_TEST_CHECK(even_total == 6);
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        bool cancelled_called = false;
        event_hub::CancellationSource source;
        event_hub::AwaitOptions options;
        options.token = source.token();

        auto awaiter = endpoint.await_once<Ping>(
            [&cancelled_called](const Ping&) {
                cancelled_called = true;
            },
            options);

        source.cancel();
        EVENT_HUB_TEST_CHECK(options.token.is_cancelled());

        source.reset();
        EVENT_HUB_TEST_CHECK(!source.token().is_cancelled());
        EVENT_HUB_TEST_CHECK(options.token.is_cancelled());

        endpoint.emit_direct<Ping>(1);

        EVENT_HUB_TEST_CHECK(!cancelled_called);
        EVENT_HUB_TEST_CHECK(awaiter->is_active());
        EVENT_HUB_TEST_CHECK(bus.process() == 0);
        EVENT_HUB_TEST_CHECK(!cancelled_called);
        EVENT_HUB_TEST_CHECK(!awaiter->is_active());
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        bool timed_out = false;
        auto options = event_hub::AwaitOptions::timeout_ms(1);
        options.on_timeout = [&timed_out] {
            timed_out = true;
        };

        auto awaiter = endpoint.await_once<Message>(
            [](const Message&) {
                EVENT_HUB_TEST_CHECK(false && "timeout awaiter should not receive messages");
            },
            options);

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        endpoint.emit_direct<Ping>(1);
        EVENT_HUB_TEST_CHECK(!timed_out);
        EVENT_HUB_TEST_CHECK(awaiter->is_active());

        EVENT_HUB_TEST_CHECK(bus.process() == 0);
        EVENT_HUB_TEST_CHECK(timed_out);
        EVENT_HUB_TEST_CHECK(!awaiter->is_active());
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        bool timed_out = false;
        auto options = event_hub::AwaitOptions::timeout_ms(1);
        options.set_delivery(event_hub::DeliveryPolicy::direct);
        options.on_timeout = [&timed_out] {
            timed_out = true;
        };

        auto awaiter = endpoint.await_once<Message>(
            [](const Message&) {
                EVENT_HUB_TEST_CHECK(false && "direct timeout awaiter should not receive messages");
            },
            options);

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        EVENT_HUB_TEST_CHECK(bus.process() == 0);
        EVENT_HUB_TEST_CHECK(!timed_out);
        EVENT_HUB_TEST_CHECK(awaiter->is_active());

        endpoint.emit_direct<Ping>(1);
        EVENT_HUB_TEST_CHECK(timed_out);
        EVENT_HUB_TEST_CHECK(!awaiter->is_active());
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        int calls = 0;

        auto awaiter = endpoint.await_each<Ping>([&calls](const Ping&) {
            ++calls;
        });

        endpoint.close();
        endpoint.close();

        EVENT_HUB_TEST_CHECK(endpoint.is_closed());
        EVENT_HUB_TEST_CHECK(!awaiter->is_active());

        bus.emit_direct<Ping>(1);
        EVENT_HUB_TEST_CHECK(calls == 0);
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        endpoint.close();

        EVENT_HUB_TEST_CHECK(endpoint.subscribe_queued<Ping>(
                                 [](const Ping&) {}) == 0U);
        EVENT_HUB_TEST_CHECK(!endpoint.await_once<Ping>(
            [](const Ping&) {}));
        EVENT_HUB_TEST_CHECK(!endpoint.await_each<Ping>(
            [](const Ping&) {}));

        endpoint.post<Ping>(1);
        const auto result = endpoint.emit_direct<Ping>(1);
        EVENT_HUB_TEST_CHECK(result.matched == 0U);
        EVENT_HUB_TEST_CHECK(result.delivered == 0U);
        EVENT_HUB_TEST_CHECK(bus.process() == 0U);
    }

    {
        event_hub::EventBus bus;
        int direct_total = 0;
        event_hub::AwaitOptions direct_options;
        direct_options.set_delivery(event_hub::DeliveryPolicy::direct);

        auto awaiter = event_hub::EventAwaiter<Ping>::create(
            bus,
            [](const Ping& ping) { return ping.value > 0; },
            [&direct_total](const Ping& ping) {
                direct_total += ping.value;
            },
            direct_options,
            true);

        bus.emit_direct<Ping>(5);
        bus.emit_direct<Ping>(6);

        EVENT_HUB_TEST_CHECK(direct_total == 5);
        EVENT_HUB_TEST_CHECK(!awaiter->is_active());
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        int queued_total = 0;

        auto awaiter = endpoint.await_once<Ping>(
            [&queued_total](const Ping& ping) {
                queued_total += ping.value;
            });

        endpoint.emit_direct<Ping>(3);
        EVENT_HUB_TEST_CHECK(queued_total == 0);
        EVENT_HUB_TEST_CHECK(awaiter->is_active());

        endpoint.post<Ping>(4);
        EVENT_HUB_TEST_CHECK(bus.process() == 1);
        EVENT_HUB_TEST_CHECK(queued_total == 4);
        EVENT_HUB_TEST_CHECK(!awaiter->is_active());
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        int exception_count = 0;

        bus.set_exception_handler([&exception_count](std::exception_ptr exception) {
            try {
                if (exception) {
                    std::rethrow_exception(exception);
                }
            } catch (const std::runtime_error&) {
                ++exception_count;
            }
        });

        event_hub::AwaitOptions options;
        options.set_timeout_ms(1);
        options.on_timeout = [] {
            throw std::runtime_error("timeout failed");
        };

        endpoint.await_once<Message>(
            [](const Message&) {
                EVENT_HUB_TEST_CHECK(false && "timeout awaiter should not receive messages");
            },
            options);

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        EVENT_HUB_TEST_CHECK(bus.process() == 0);
        EVENT_HUB_TEST_CHECK(exception_count == 1);
    }

    return 0;
}
