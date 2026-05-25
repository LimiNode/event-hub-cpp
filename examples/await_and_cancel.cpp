#include <event_hub.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

struct JobEvent {
    std::string id;
    int progress = 0;
};

struct TimeoutEvent {
    std::string name;
};

int main() {
    using namespace std::chrono_literals;

    event_hub::EventBus bus;
    event_hub::EventEndpoint endpoint(bus);

    // await_once() keeps an internal subscription until the predicate matches.
    // This handler runs once for the first JobEvent that reaches 100%.
    endpoint.await_once<JobEvent>(
        [](const JobEvent& event) {
            return event.progress == 100;
        },
        [](const JobEvent& event) {
            std::cout << "job finished: " << event.id << '\n';
        });

    // await_each() keeps listening until the returned handle is cancelled, the
    // endpoint is closed, a token cancels it, or a timeout expires.
    auto progress_stream = endpoint.await_each<JobEvent>(
        [](const JobEvent& event) {
            std::cout << "progress: " << event.id << ' ' << event.progress << '\n';
        });

    endpoint.post<JobEvent>("compile", 10);
    endpoint.post<JobEvent>("compile", 100);
    // process() dispatches queued events and then polls awaiters for timeout
    // and cancellation state. Awaiter callbacks run on this same thread.
    bus.process();

    // The stream handle owns cancellation for await_each(). After cancel(),
    // later queued JobEvent deliveries no longer reach the progress callback.
    progress_stream->cancel();
    endpoint.post<JobEvent>("compile", 200);
    bus.process();

    event_hub::CancellationSource source;
    event_hub::AwaitOptions cancelled_options;
    cancelled_options.token = source.token();
    // Cancellation tokens are cooperative: the awaiter observes cancellation
    // when the bus next reaches a poll point accepted by the awaiter delivery
    // policy. Awaiters use queued delivery by default.
    endpoint.await_once<JobEvent>(
        [](const JobEvent&) {
            std::cout << "this line is cancelled\n";
        },
        cancelled_options);
    source.cancel();
    // process() is the queued poll point where the cancelled token is seen.
    bus.process();

    auto timeout_options = event_hub::AwaitOptions::timeout_ms(1);
    // Timeouts do not start a background thread. The callback fires when the
    // bus is polled after the deadline has passed from an accepted delivery
    // source. Awaiters use queued delivery by default, so process() invokes it.
    timeout_options.on_timeout = [] {
        std::cout << "timeout fired\n";
    };

    endpoint.await_once<TimeoutEvent>(
        [](const TimeoutEvent&) {
            std::cout << "this line should not print\n";
        },
        timeout_options);

    std::this_thread::sleep_for(2ms);
    // No TimeoutEvent is posted; process() is the poll point that notices the
    // expired timeout and invokes on_timeout.
    bus.process();
}
