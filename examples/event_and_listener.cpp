#include <event_hub.hpp>

#include <iostream>
#include <string>
#include <utility>

class SettingsReloadedEvent final : public event_hub::Event {
public:
    explicit SettingsReloadedEvent(std::string file)
        : file(std::move(file)) {}

    EVENT_HUB_EVENT(SettingsReloadedEvent)

    std::string file;
};

class SettingsReloadFailedEvent final : public event_hub::Event {
public:
    SettingsReloadFailedEvent(std::string file, std::string reason)
        : file(std::move(file)),
          reason(std::move(reason)) {}

    EVENT_HUB_EVENT(SettingsReloadFailedEvent)

    std::string file;
    std::string reason;
};

class AuditListener final : public event_hub::EventListener {
public:
    void on_event(const event_hub::Event& event) override {
        if (const auto* reloaded = event.as<SettingsReloadedEvent>()) {
            std::cout << event.name() << ": " << reloaded->file << '\n';
            return;
        }

        if (const auto* failed = event.as<SettingsReloadFailedEvent>()) {
            std::cout << event.name() << ": " << failed->file
                      << ", reason=" << failed->reason << '\n';
        }
    }
};

int main() {
    event_hub::EventBus bus;
    event_hub::EventEndpoint endpoint(bus);
    AuditListener audit;

    endpoint.subscribe_direct<SettingsReloadedEvent>(audit);
    endpoint.subscribe_direct<SettingsReloadFailedEvent>(audit);

    endpoint.emit_direct<SettingsReloadedEvent>("app.json");
    endpoint.emit_direct<SettingsReloadFailedEvent>("runtime.json", "file not found");

    SettingsReloadFailedEvent event("override.json", "invalid json");
    std::cout << "is SettingsReloadedEvent: "
              << event.is<SettingsReloadedEvent>() << '\n';
    std::cout << "is SettingsReloadFailedEvent: "
              << event.is<SettingsReloadFailedEvent>() << '\n';
}
