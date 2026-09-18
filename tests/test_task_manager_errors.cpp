#include "test_helpers.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace event_hub_test;

int main() {
    {
        event_hub::TaskManager tasks;
        CountingNotifier notifier;
        tasks.set_notifier(&notifier);

        int value = 0;
        std::vector<event_hub::Task> batch;
        batch.emplace_back([&value] {
            value += 1;
        });
        batch.emplace_back(nullptr);
        batch.emplace_back([&value] {
            value += 2;
        });

        const auto ids = tasks.post_batch(std::move(batch));
        EVENT_HUB_TEST_CHECK(ids.size() == 3);
        EVENT_HUB_TEST_CHECK(ids[0] != 0);
        EVENT_HUB_TEST_CHECK(ids[1] == 0);
        EVENT_HUB_TEST_CHECK(ids[2] != 0);
        EVENT_HUB_TEST_CHECK(notifier.notifications.load(std::memory_order_relaxed) == 1);

        EVENT_HUB_TEST_CHECK(tasks.process() == 2);
        EVENT_HUB_TEST_CHECK(value == 3);
        tasks.reset_notifier();
    }

    {
        event_hub::TaskManager tasks;
        int value = 0;

        auto owned = std::make_unique<int>(7);
        const auto id = tasks.post([ptr = std::move(owned), &value] {
            value = *ptr;
        });
        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(tasks.process() == 1);
        EVENT_HUB_TEST_CHECK(value == 7);

        auto future = tasks.submit([] {
            return 42;
        });
        EVENT_HUB_TEST_CHECK(future.wait_for(std::chrono::milliseconds(0)) ==
               std::future_status::timeout);
        EVENT_HUB_TEST_CHECK(tasks.process() == 1);
        EVENT_HUB_TEST_CHECK(future.get() == 42);

        auto void_future = tasks.submit([&value] {
            value = 9;
        });
        EVENT_HUB_TEST_CHECK(tasks.process() == 1);
        void_future.get();
        EVENT_HUB_TEST_CHECK(value == 9);
    }

    {
        event_hub::TaskManager tasks;
        int exception_count = 0;
        int value = 0;

        tasks.set_exception_handler(
            [&exception_count](std::exception_ptr exception) {
                try {
                    if (exception) {
                        std::rethrow_exception(exception);
                    }
                } catch (const std::runtime_error&) {
                    ++exception_count;
                }
            });

        tasks.post([] {
            throw std::runtime_error("task failed");
        });
        tasks.post([&value] {
            value = 5;
        });

        EVENT_HUB_TEST_CHECK(tasks.process() == 2);
        EVENT_HUB_TEST_CHECK(exception_count == 1);
        EVENT_HUB_TEST_CHECK(value == 5);
    }

    {
        event_hub::TaskManager tasks;
        std::vector<int> seen;

        tasks.post([&tasks, &seen] {
            seen.push_back(1);
            tasks.post([&seen] {
                seen.push_back(4);
            });
            throw std::runtime_error("task failed");
        });
        tasks.post([&seen] {
            seen.push_back(2);
        });
        tasks.post([&seen] {
            seen.push_back(3);
        });

        bool threw = false;
        try {
            (void)tasks.process();
        } catch (const std::runtime_error&) {
            threw = true;
        }

        EVENT_HUB_TEST_CHECK(threw);
        EVENT_HUB_TEST_CHECK(tasks.pending_count() == 3);
        EVENT_HUB_TEST_CHECK(tasks.ready_count() == 3);
        EVENT_HUB_TEST_CHECK(tasks.process() == 3);
        EVENT_HUB_TEST_CHECK((seen == std::vector<int>{1, 2, 3, 4}));
    }

    {
        event_hub::TaskManager tasks;
        std::function<void()> empty_function;

        EVENT_HUB_TEST_CHECK(tasks.post_every_ms(0, [] {}) == 0);
        EVENT_HUB_TEST_CHECK(tasks.post_every_ms(-1, [] {}) == 0);
        EVENT_HUB_TEST_CHECK(tasks.post_every_after_ms(0, 0, [] {}) == 0);
        EVENT_HUB_TEST_CHECK(tasks.post_every_ms(1, empty_function) == 0);

        tasks.close();
        EVENT_HUB_TEST_CHECK(tasks.post_every_ms(1, [] {}) == 0);
    }

    {
        event_hub::TaskManager tasks;
        std::function<void()> empty_function;

        EVENT_HUB_TEST_CHECK(tasks.add_task_at_system(
            event_hub::TaskManager::SystemClock::now(),
            empty_function) == 0);
        EVENT_HUB_TEST_CHECK(tasks.add_task_at_system_ms(0,
                                                         empty_function) == 0);

        tasks.close();
        EVENT_HUB_TEST_CHECK(tasks.add_task_at_system(
            event_hub::TaskManager::SystemClock::now(),
            [] {}) == 0);
        EVENT_HUB_TEST_CHECK(tasks.add_task_at_system_ms(0, [] {}) == 0);
    }

    {
        event_hub::TaskManager tasks;
        int calls = 0;

        const auto id = tasks.post_every_ms(1, [&calls] {
            ++calls;
            throw std::runtime_error("periodic task failed");
        });

        bool threw = false;
        try {
            (void)tasks.process();
        } catch (const std::runtime_error&) {
            threw = true;
        }

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(threw);
        EVENT_HUB_TEST_CHECK(calls == 1);
        EVENT_HUB_TEST_CHECK(!tasks.has_pending());
        EVENT_HUB_TEST_CHECK(!tasks.cancel(id));
        EVENT_HUB_TEST_CHECK(tasks.process() == 0);
    }

    {
        event_hub::TaskManager tasks;
        int calls = 0;
        int exception_count = 0;

        tasks.set_exception_handler(
            [&exception_count](std::exception_ptr exception) {
                try {
                    if (exception) {
                        std::rethrow_exception(exception);
                    }
                } catch (const std::runtime_error&) {
                    ++exception_count;
                }
            });

        const auto id = tasks.post_every_ms(1, [&calls] {
            ++calls;
            throw std::runtime_error("periodic task failed");
        });

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(tasks.process() == 1);
        EVENT_HUB_TEST_CHECK(calls == 1);
        EVENT_HUB_TEST_CHECK(exception_count == 1);
        EVENT_HUB_TEST_CHECK(!tasks.has_pending());
        EVENT_HUB_TEST_CHECK(!tasks.cancel(id));
        EVENT_HUB_TEST_CHECK(tasks.process() == 0);
    }

    {
        event_hub::TaskManager tasks;
        int calls = 0;

        const auto id = tasks.post([&calls](event_hub::TaskContext& self) {
            ++calls;
            EVENT_HUB_TEST_CHECK(self.reschedule_after(
                std::chrono::milliseconds(1)));
            throw std::runtime_error("rescheduled task failed");
        });

        bool threw = false;
        try {
            (void)tasks.process();
        } catch (const std::runtime_error&) {
            threw = true;
        }

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(threw);
        EVENT_HUB_TEST_CHECK(calls == 1);
        EVENT_HUB_TEST_CHECK(!tasks.has_pending());

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        EVENT_HUB_TEST_CHECK(tasks.process() == 0);
    }

    {
        event_hub::SyncNotifier notifier;
        event_hub::TaskManager tasks(&notifier);

        const auto generation = notifier.generation();
        const auto id = tasks.post([] {});

        EVENT_HUB_TEST_CHECK(id != 0);
        EVENT_HUB_TEST_CHECK(notifier.wait_for_ms(generation, 0));
        EVENT_HUB_TEST_CHECK(tasks.process() == 1);
        tasks.reset_notifier();
    }

    {
        event_hub::TaskManager tasks;

        EVENT_HUB_TEST_CHECK(tasks.recommend_wait_for(std::chrono::milliseconds(10)) ==
               event_hub::TaskManager::Duration(std::chrono::milliseconds(10)));
        EVENT_HUB_TEST_CHECK(tasks.recommend_wait_for_ms(10) ==
               event_hub::TaskManager::Duration(std::chrono::milliseconds(10)));
        EVENT_HUB_TEST_CHECK(!tasks.next_deadline());

        tasks.post_after_ms(30, [] {});
        const auto delayed_wait =
            tasks.recommend_wait_for_ms(100);

        EVENT_HUB_TEST_CHECK(tasks.next_deadline());
        EVENT_HUB_TEST_CHECK(delayed_wait > event_hub::TaskManager::Duration::zero());
        EVENT_HUB_TEST_CHECK(delayed_wait <=
               event_hub::TaskManager::Duration(std::chrono::milliseconds(100)));

        tasks.post([] {});
        EVENT_HUB_TEST_CHECK(tasks.recommend_wait_for_ms(10) ==
               event_hub::TaskManager::Duration::zero());

        EVENT_HUB_TEST_CHECK(tasks.clear_pending() == 2);
        EVENT_HUB_TEST_CHECK(!tasks.has_pending());
    }

    {
        event_hub::TaskManager tasks;
        std::vector<int> handler_state;
        tasks.set_exception_handler(
            [count = 0, &handler_state](std::exception_ptr) mutable {
                handler_state.push_back(++count);
            });
        tasks.post([] { throw std::runtime_error("first"); });
        tasks.post([] { throw std::runtime_error("second"); });

        EVENT_HUB_TEST_CHECK(tasks.process() == 2);
        EVENT_HUB_TEST_CHECK((handler_state == std::vector<int>{1, 2}));
    }

    return 0;
}
