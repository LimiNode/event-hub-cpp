#include "test_helpers.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace event_hub_test;

namespace {

constexpr int producer_count = 8;
constexpr int items_per_producer = 128;
constexpr int expected_total = producer_count * items_per_producer;

} // namespace

int main() {
    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        std::atomic_int received{0};

        endpoint.subscribe_queued<Ping>([&](const Ping& ping) {
            received.fetch_add(ping.value, std::memory_order_relaxed);
        });

        std::vector<std::thread> producers;
        for (int producer = 0; producer < producer_count; ++producer) {
            producers.emplace_back([&endpoint] {
                for (int i = 0; i < items_per_producer; ++i) {
                    endpoint.post<Ping>(1);
                }
            });
        }

        for (auto& producer : producers) {
            producer.join();
        }

        EVENT_HUB_TEST_CHECK(bus.pending_count() ==
                             static_cast<std::size_t>(expected_total));
        EVENT_HUB_TEST_CHECK(bus.process() ==
                             static_cast<std::size_t>(expected_total));
        EVENT_HUB_TEST_CHECK(received.load(std::memory_order_relaxed) ==
                             expected_total);
    }

    {
        event_hub::TaskManager tasks;
        std::atomic_int value{0};

        std::vector<std::thread> producers;
        for (int producer = 0; producer < producer_count; ++producer) {
            producers.emplace_back([&tasks, &value] {
                for (int i = 0; i < items_per_producer; ++i) {
                    const auto id = tasks.post([&value] {
                        value.fetch_add(1, std::memory_order_relaxed);
                    });
                    EVENT_HUB_TEST_CHECK(id != 0);
                }
            });
        }

        for (auto& producer : producers) {
            producer.join();
        }

        EVENT_HUB_TEST_CHECK(tasks.pending_count() ==
                             static_cast<std::size_t>(expected_total));
        EVENT_HUB_TEST_CHECK(tasks.process() ==
                             static_cast<std::size_t>(expected_total));
        EVENT_HUB_TEST_CHECK(value.load(std::memory_order_relaxed) ==
                             expected_total);
    }

    {
        event_hub::TaskManager tasks;
        std::vector<event_hub::TaskId> ids;
        ids.reserve(expected_total);

        for (int i = 0; i < expected_total; ++i) {
            const auto id = tasks.post([] {});
            EVENT_HUB_TEST_CHECK(id != 0);
            ids.push_back(id);
        }

        std::atomic_int cancelled{0};
        std::vector<std::thread> cancellers;
        for (int producer = 0; producer < producer_count; ++producer) {
            cancellers.emplace_back([&, producer] {
                for (int index = producer;
                     index < expected_total;
                     index += producer_count) {
                    if (tasks.cancel(ids[static_cast<std::size_t>(index)])) {
                        cancelled.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& canceller : cancellers) {
            canceller.join();
        }

        EVENT_HUB_TEST_CHECK(cancelled.load(std::memory_order_relaxed) ==
                             expected_total);
        EVENT_HUB_TEST_CHECK(tasks.process() == 0);
        EVENT_HUB_TEST_CHECK(!tasks.has_pending());
    }

    return 0;
}
