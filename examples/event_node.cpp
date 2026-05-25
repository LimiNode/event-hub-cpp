#include <event_hub.hpp>

#include <iostream>
#include <string>
#include <utility>

struct TokenFoundEvent final : event_hub::Event {
    std::string token;
    std::string source;

    TokenFoundEvent(std::string token_, std::string source_)
        : token(std::move(token_)),
          source(std::move(source_)) {}

    EVENT_HUB_EVENT(TokenFoundEvent)
};

struct TokenRejectedEvent final : event_hub::Event {
    std::string token;
    std::string source;
    std::string reason;

    TokenRejectedEvent(std::string token_,
                       std::string source_,
                       std::string reason_)
        : token(std::move(token_)),
          source(std::move(source_)),
          reason(std::move(reason_)) {}

    EVENT_HUB_EVENT(TokenRejectedEvent)
};

// Plain C++ event: no event_hub::Event base and no EVENT_HUB_EVENT metadata.
struct ScanSummary {
    int accepted = 0;
    int rejected = 0;

    ScanSummary(int accepted_, int rejected_)
        : accepted(accepted_),
          rejected(rejected_) {}
};

class TokenModule final : public event_hub::EventNode {
public:
    explicit TokenModule(event_hub::EventBus& bus)
        : EventNode(bus) {}

    void start() {
        m_found_subscription = listen_any<TokenFoundEvent>();
        listen_queued<TokenRejectedEvent>();

        // Plain events use callback subscriptions, not listen<T>().
        subscribe_queued<ScanSummary>([](const ScanSummary& summary) {
            std::cout << "plain summary accepted=" << summary.accepted
                      << ", rejected=" << summary.rejected << '\n';
        });
    }

    void publish_immediate_token() {
        emit_direct<TokenFoundEvent>("sync-token", "memory");
    }

    void publish_test_events() {
        post<TokenFoundEvent>("abc123", "github");
        post<TokenRejectedEvent>("x", "github", "too short");
        post<ScanSummary>(1, 1);
    }

    void unsubscribe_from_found_tokens() {
        unsubscribe(m_found_subscription);
        m_found_subscription = 0;
    }

    void unsubscribe_from_summaries() {
        unsubscribe<ScanSummary>();
    }

    void on_event(const event_hub::Event& event) override {
        if (const auto* found = event.as<TokenFoundEvent>()) {
            std::cout << "token=" << found->token
                      << ", source=" << found->source << '\n';
            return;
        }

        if (const auto* rejected = event.as<TokenRejectedEvent>()) {
            std::cout << "rejected=" << rejected->token
                      << ", source=" << rejected->source
                      << ", reason=" << rejected->reason << '\n';
        }
    }

private:
    SubscriptionId m_found_subscription = 0;
};

int main() {
    event_hub::EventBus bus;

    TokenModule module(bus);
    module.start();

    module.publish_immediate_token();

    module.publish_test_events();
    bus.process();

    module.unsubscribe_from_found_tokens();
    module.unsubscribe_from_summaries();

    module.publish_test_events();
    bus.process();

    module.close();
    std::cout << "closed=" << module.is_closed() << '\n';

    bus.post<TokenRejectedEvent>("late", "github", "module closed");
    bus.process();

    return 0;
}
