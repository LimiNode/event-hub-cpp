#include "test_helpers.hpp"

#include <memory>
#include <string>
#include <typeinfo>

using namespace event_hub_test;

namespace {

class PlainValueNode final : public event_hub::EventNode {
public:
    explicit PlainValueNode(event_hub::EventBus& bus)
        : EventNode(bus) {}

    void start() {
        m_subscription = subscribe_any<Ping>([this](const Ping& ping) {
            total += ping.value;
        });
    }

    void stop_one() {
        unsubscribe(m_subscription);
    }

    void publish(int value) {
        post<Ping>(value);
    }

    void publish_event(const Ping& event) {
        post(event);
    }

    void on_event(const event_hub::Event&) override {}

    int total = 0;

private:
    SubscriptionId m_subscription = 0;
};

} // namespace

int main() {
    {
        DerivedEvent event(5);

        EVENT_HUB_TEST_CHECK(event.is<DerivedEvent>());
        EVENT_HUB_TEST_CHECK(!event.is<OtherEvent>());
        EVENT_HUB_TEST_CHECK(std::string(event.name()) == "DerivedEvent");
        EVENT_HUB_TEST_CHECK(event.as<DerivedEvent>() == &event);
        EVENT_HUB_TEST_CHECK(event.as<OtherEvent>() == nullptr);

        auto clone = event.clone();
        EVENT_HUB_TEST_CHECK(clone->is<DerivedEvent>());
        EVENT_HUB_TEST_CHECK(clone->as_ref<DerivedEvent>().value == 5);

        bool bad_cast = false;
        try {
            (void)clone->as_ref<OtherEvent>();
        } catch (const std::bad_cast&) {
            bad_cast = true;
        }
        EVENT_HUB_TEST_CHECK(bad_cast);
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        CountingListener listener;

        endpoint.subscribe_direct<DerivedEvent>(listener);
        endpoint.emit_direct<DerivedEvent>(5);

        EVENT_HUB_TEST_CHECK(listener.total == 5);
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        CountingListener listener;

        endpoint.subscribe_queued<DerivedEvent>(listener);
        endpoint.emit_direct<DerivedEvent>(5);
        EVENT_HUB_TEST_CHECK(listener.total == 0);

        endpoint.post<DerivedEvent>(7);
        EVENT_HUB_TEST_CHECK(bus.process() == 1);
        EVENT_HUB_TEST_CHECK(listener.total == 7);
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        CountingListener listener;
        auto listener_guard = std::make_shared<int>(0);

        endpoint.subscribe_direct<DerivedEvent>(
            std::weak_ptr<int>(listener_guard),
            listener);

        listener_guard.reset();
        endpoint.emit_direct<DerivedEvent>(7);

        EVENT_HUB_TEST_CHECK(listener.total == 0);
    }

    {
        event_hub::EventBus bus;
        TestNode node(bus);

        EVENT_HUB_TEST_CHECK(&node.bus() == &bus);
        EVENT_HUB_TEST_CHECK(!node.is_closed());

        node.start();

        bus.post<NodeTestEvent>(3);
        EVENT_HUB_TEST_CHECK(bus.process() == 1);
        EVENT_HUB_TEST_CHECK(node.received == 3);

        node.close();
        EVENT_HUB_TEST_CHECK(node.is_closed());

        bus.post<NodeTestEvent>(5);
        EVENT_HUB_TEST_CHECK(bus.process() == 1);
        EVENT_HUB_TEST_CHECK(node.received == 3);
    }

    {
        event_hub::EventBus bus;
        PlainValueNode node(bus);

        node.start();
        bus.emit_direct<Ping>(2);
        EVENT_HUB_TEST_CHECK(node.total == 2);

        node.publish(3);
        EVENT_HUB_TEST_CHECK(bus.process() == 1);
        EVENT_HUB_TEST_CHECK(node.total == 5);

        Ping event{4};
        node.publish_event(event);
        event.value = 100;
        EVENT_HUB_TEST_CHECK(bus.process() == 1);
        EVENT_HUB_TEST_CHECK(node.total == 9);

        node.stop_one();
        bus.emit_direct<Ping>(4);
        EVENT_HUB_TEST_CHECK(node.total == 9);
    }

    return 0;
}
