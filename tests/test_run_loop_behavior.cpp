#include "test_helpers.hpp"

#include <chrono>
#include <string>
#include <vector>

using namespace event_hub_test;

int main() {
    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        event_hub::TaskManager tasks;
        event_hub::RunLoop loop(0);
        std::vector<std::string> messages;
        std::vector<int> task_values;

        EVENT_HUB_TEST_CHECK(loop.max_tasks_per_manager() == 1);

        endpoint.subscribe_queued<Message>([&messages](const Message& message) {
            messages.push_back(message.text);
        });

        loop.add_bus(bus);
        loop.add(bus);
        loop.add_task_manager(tasks);
        loop.add(tasks);

        endpoint.post<Message>("event");
        tasks.post([&task_values] {
            task_values.push_back(1);
        });
        tasks.post([&task_values] {
            task_values.push_back(2);
        });

        EVENT_HUB_TEST_CHECK(loop.process_once() == 2);
        EVENT_HUB_TEST_CHECK((messages == std::vector<std::string>{"event"}));
        EVENT_HUB_TEST_CHECK((task_values == std::vector<int>{1}));

        EVENT_HUB_TEST_CHECK(loop.process_once() == 1);
        EVENT_HUB_TEST_CHECK((task_values == std::vector<int>{1, 2}));

        const auto generation = loop.notifier().generation();
        loop.reset_sources();
        endpoint.post<Message>("manual");
        EVENT_HUB_TEST_CHECK(!loop.notifier().wait_for(generation,
                                         std::chrono::milliseconds(0)));
        EVENT_HUB_TEST_CHECK(bus.process() == 1);
        EVENT_HUB_TEST_CHECK((messages == std::vector<std::string>{"event", "manual"}));
    }

    {
        event_hub::RunLoop loop;
        bool predicate_called = false;

        loop.run_until([&predicate_called] {
            predicate_called = true;
            return true;
        });

        EVENT_HUB_TEST_CHECK(predicate_called);
        EVENT_HUB_TEST_CHECK(!loop.stop_requested());

        loop.request_stop();
        EVENT_HUB_TEST_CHECK(loop.stop_requested());
        loop.reset_stop();
        EVENT_HUB_TEST_CHECK(!loop.stop_requested());
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        event_hub::TaskManager first_tasks;
        event_hub::TaskManager second_tasks;
        event_hub::RunLoop loop;
        std::vector<std::string> messages;

        endpoint.subscribe_queued<Message>([&messages, &loop](const Message& message) {
            messages.push_back(message.text);
            if (message.text == "second") {
                loop.request_stop();
            }
        });

        loop.add(bus);
        loop.add(first_tasks);
        loop.add(second_tasks);

        first_tasks.post([&endpoint] {
            endpoint.post<Message>("first");
        });
        second_tasks.post_after(std::chrono::milliseconds(2), [&endpoint] {
            endpoint.post<Message>("second");
        });

        const auto deadline =
            event_hub::RunLoop::Clock::now() + std::chrono::seconds(2);
        loop.run_until([deadline] {
            return event_hub::RunLoop::Clock::now() >= deadline;
        });

        EVENT_HUB_TEST_CHECK(loop.stop_requested());
        EVENT_HUB_TEST_CHECK((messages == std::vector<std::string>{"first", "second"}));
    }

    {
        event_hub::EventBus bus;
        event_hub::EventEndpoint endpoint(bus);
        event_hub::TaskManager tasks;
        event_hub::RunLoop loop;
        bool timed_out = false;

        auto options = event_hub::AwaitOptions::timeout_ms(20);
        options.on_timeout = [&timed_out] { timed_out = true; };
        endpoint.await_once<Message>([](const Message&) {}, std::move(options));

        loop.add(bus);
        loop.add(tasks);
        tasks.post_after(std::chrono::milliseconds(100), [&loop] {
            loop.request_stop();
        });

        loop.run();
        EVENT_HUB_TEST_CHECK(timed_out);
    }

    return 0;
}
