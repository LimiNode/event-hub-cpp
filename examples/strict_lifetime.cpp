#include <event_hub.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

struct TokenFoundEvent {
    std::string source;
    std::string value;
};

class TokenModule final : public std::enable_shared_from_this<TokenModule> {
public:
    explicit TokenModule(event_hub::EventBus& bus)
        : m_endpoint(bus) {}

    void start() {
        auto weak = weak_from_this();

        m_endpoint.subscribe_queued<TokenFoundEvent>(
            weak,
            [weak](const TokenFoundEvent& event) {
                auto self = weak.lock();
                if (!self) {
                    return;
                }

                self->on_token_found(event);
            });
    }

    const std::vector<std::string>& tokens() const {
        return m_tokens;
    }

private:
    void on_token_found(const TokenFoundEvent& event) {
        m_tokens.push_back(event.source + ":" + event.value);
    }

private:
    event_hub::EventEndpoint m_endpoint;
    std::vector<std::string> m_tokens;
};

int main() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint producer(bus);

    auto module = std::make_shared<TokenModule>(bus);
    module->start();

    producer.post<TokenFoundEvent>("scanner", "token-1");
    bus.process();

    for (const auto& token : module->tokens()) {
        std::cout << token << '\n';
    }

    module.reset();

    producer.post<TokenFoundEvent>("scanner", "late-token");
    bus.process();

    std::cout << "late event skipped after module reset\n";
}
