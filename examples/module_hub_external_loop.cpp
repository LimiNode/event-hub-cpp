#include <event_hub.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

using namespace std::chrono_literals;

struct FeedEvent {
    std::string symbol;
    double price = 0.0;
};

struct ReportEvent {
    std::string text;
};

struct StopEvent {};

class FeedModule final : public event_hub::Module {
public:
    explicit FeedModule(event_hub::EventBus& bus)
        : Module(bus) {}

private:
    void on_initialize() override {
        subscribe_queued<FeedEvent>([this](const FeedEvent& event) {
            std::cout << "feed: " << event.symbol << " = "
                      << event.price << '\n';

            tasks().post([this, event] {
                post<ReportEvent>("processed " + event.symbol);
            });
        });
    }
};

class ManualReportModule final : public event_hub::Module {
public:
    explicit ManualReportModule(event_hub::EventBus& bus)
        : Module(bus,
                 {event_hub::ModuleExecutionMode::manual, 8}) {}

    void queue_report(std::string text) {
        tasks().post([this, text = std::move(text)] {
            post<ReportEvent>("manual: " + text);
        });
    }
};

class ExternalFeedAdapter {
public:
    ExternalFeedAdapter(event_hub::INotifier& notifier,
                        event_hub::EventBus& bus)
        : tasks(&notifier),
          endpoint(bus) {}

    void start() {
        tasks.post_after(1ms, [this] {
            endpoint.post<FeedEvent>("EURUSD", 1.0852);
        });

        tasks.post_after(3ms, [this] {
            endpoint.post<FeedEvent>("BTCUSD", 62150.0);
        });

        tasks.post_after(6ms, [this] {
            endpoint.post<StopEvent>();
        });
    }

    event_hub::TaskManager tasks;

private:
    event_hub::EventEndpoint endpoint;
};

std::optional<event_hub::TaskManager::TimePoint> earliest(
    std::optional<event_hub::TaskManager::TimePoint> lhs,
    std::optional<event_hub::TaskManager::TimePoint> rhs) {
    if (!lhs) {
        return rhs;
    }
    if (!rhs) {
        return lhs;
    }
    return std::min(*lhs, *rhs);
}

event_hub::TaskManager::Duration wait_until(
    std::optional<event_hub::TaskManager::TimePoint> deadline,
    event_hub::TaskManager::Duration idle_cap) {
    if (!deadline) {
        return idle_cap;
    }

    const auto now = event_hub::TaskManager::Clock::now();
    if (*deadline <= now) {
        return event_hub::TaskManager::Duration::zero();
    }

    const auto wait = *deadline - now;
    return wait < idle_cap ? wait : idle_cap;
}

int main() {
    event_hub::SyncNotifier notifier;
    event_hub::ModuleHub hub;

    auto& manual_reports = hub.emplace_module<ManualReportModule>();
    hub.emplace_module<FeedModule>();

    event_hub::EventEndpoint output(hub.bus());
    bool running = true;

    output.subscribe_queued<ReportEvent>([](const ReportEvent& event) {
        std::cout << "report: " << event.text << '\n';
    });

    output.subscribe_queued<StopEvent>([&running](const StopEvent&) {
        running = false;
    });

    ExternalFeedAdapter external_feed(notifier, hub.bus());

    hub.set_notifier(&notifier);
    manual_reports.tasks().set_notifier(&notifier);

    hub.initialize();
    external_feed.start();
    manual_reports.queue_report("external loop owns this queue");

    while (running || hub.has_pending() ||
           external_feed.tasks.has_pending() ||
           manual_reports.tasks().has_pending()) {
        const auto generation = notifier.generation();

        std::size_t work_done = 0;
        work_done += hub.process();
        work_done += external_feed.tasks.process(16);
        work_done += manual_reports.tasks().process(16);

        if (work_done != 0U) {
            continue;
        }

        const auto deadline = earliest(
            earliest(hub.next_deadline(), external_feed.tasks.next_deadline()),
            manual_reports.tasks().next_deadline());

        if (!hub.has_pending() && !external_feed.tasks.has_ready() &&
            !manual_reports.tasks().has_ready()) {
            (void)notifier.wait_for(generation, wait_until(deadline, 20ms));
        }
    }

    manual_reports.tasks().reset_notifier();
    external_feed.tasks.reset_notifier();
    hub.shutdown();
    hub.reset_notifier();
}
