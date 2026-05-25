#pragma once
#ifndef EVENT_HUB_EVENT_AWAITER_HPP_INCLUDED
#define EVENT_HUB_EVENT_AWAITER_HPP_INCLUDED

/// \file event_awaiter.hpp
/// \brief Defines cancelable helpers for waiting on matching events.

#include "awaiter_interfaces.hpp"
#include "cancellation.hpp"
#include "event_bus.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <utility>

namespace event_hub {

/// \brief Awaiter configuration options.
///
/// Timeout and cancellation-token state are cooperative. They are polled by
/// EventBus after emit() and process(), not by a background thread.
/// Awaiters accept queued delivery by default; set delivery explicitly when an
/// awaiter callback may run from an emit() caller thread.
struct AwaitOptions {
    std::chrono::steady_clock::duration timeout{}; ///< Zero means no timeout.
    CancellationToken token{};                     ///< Empty means no token.
    DeliveryPolicy delivery{DeliveryPolicy::queued}; ///< Accepted event delivery source.
    std::function<void()> on_timeout{};            ///< Called when timeout ends.

    /// \brief Create options with a timeout in milliseconds.
    static AwaitOptions timeout_ms(std::int64_t timeout_ms) {
        AwaitOptions options;
        options.set_timeout_ms(timeout_ms);
        return options;
    }

    /// \brief Set timeout to timeout_ms milliseconds.
    AwaitOptions& set_timeout_ms(std::int64_t timeout_ms) {
        timeout = std::chrono::milliseconds(timeout_ms);
        return *this;
    }

    /// \brief Set which delivery source can satisfy this awaiter.
    AwaitOptions& set_delivery(DeliveryPolicy value) noexcept {
        delivery = value;
        return *this;
    }
};

/// \class EventAwaiter
/// \brief Cancelable subscription that waits for events matching a predicate.
/// \tparam EventType Concrete event type to await.
///
/// EventAwaiter is usually created through EventEndpoint::await_once() or
/// EventEndpoint::await_each(). Direct creation is available for integration
/// code that needs to manage awaiter ownership itself.
///
/// \note EventBus must outlive every EventAwaiter created from it.
template <typename EventType>
class EventAwaiter final : public IAwaiterEx,
                           public std::enable_shared_from_this<EventAwaiter<EventType>> {
public:
    using Predicate = std::function<bool(const EventType&)>; ///< Predicate used to select awaited events.
    using Callback = std::function<void(const EventType&)>; ///< Callback invoked for matching events.

    /// \brief Create, subscribe, and register a new awaiter.
    /// \param bus Event bus that must outlive the awaiter.
    /// \param predicate Predicate that selects matching events.
    /// \param callback Callback invoked for matching events.
    /// \param options Timeout and cancellation options.
    /// \param single_shot True to cancel after the first match.
    /// \return Shared awaiter handle.
    static std::shared_ptr<EventAwaiter> create(EventBus& bus,
                                                Predicate predicate,
                                                Callback callback,
                                                AwaitOptions options,
                                                bool single_shot) {
        return create_internal(bus,
                               std::move(predicate),
                               std::move(callback),
                               std::move(options),
                               single_shot,
                               std::weak_ptr<void>{},
                               false);
    }

    /// \brief Create an awaiter whose bus subscription has a lifetime guard.
    /// \param bus Event bus that must outlive the awaiter.
    /// \param predicate Predicate that selects matching events.
    /// \param callback Callback invoked for matching events.
    /// \param options Timeout and cancellation options.
    /// \param single_shot True to cancel after the first match.
    /// \param guard Weak guard that must lock before the awaiter callback can
    /// run.
    /// \return Shared awaiter handle.
    static std::shared_ptr<EventAwaiter> create(EventBus& bus,
                                                Predicate predicate,
                                                Callback callback,
                                                AwaitOptions options,
                                                bool single_shot,
                                                std::weak_ptr<void> guard) {
        return create_internal(bus,
                               std::move(predicate),
                               std::move(callback),
                               std::move(options),
                               single_shot,
                               std::move(guard),
                               true);
    }

private:
    static std::shared_ptr<EventAwaiter> create_internal(EventBus& bus,
                                                        Predicate predicate,
                                                        Callback callback,
                                                        AwaitOptions options,
                                                        bool single_shot,
                                                        std::weak_ptr<void> guard,
                                                        bool has_guard) {
        auto awaiter = std::shared_ptr<EventAwaiter>(
            new EventAwaiter(bus,
                             std::move(predicate),
                             std::move(callback),
                             std::move(options),
                             single_shot,
                             std::move(guard),
                             has_guard));
        awaiter->subscribe_internal();
        bus.register_awaiter(awaiter);
        return awaiter;
    }

public:
    /// \copydoc IAwaiter::cancel
    void cancel() noexcept override {
        bool expected = false;
        if (!m_cancelled.compare_exchange_strong(expected, true,
                                                 std::memory_order_relaxed)) {
            return;
        }

        if (m_subscription_id != 0U) {
            m_bus.unsubscribe(m_subscription_id);
            m_subscription_id = 0U;
        }
        m_retain_self.reset();
    }

    /// \copydoc IAwaiter::is_active
    bool is_active() const noexcept override {
        return !m_cancelled.load(std::memory_order_relaxed);
    }

    /// \copydoc IAwaiterEx::poll_timeout
    /// \note Exceptions from on_timeout are reported to the bus exception
    /// handler when one is set; otherwise they are swallowed to preserve
    /// noexcept.
    void poll_timeout() noexcept override {
        if (!is_active()) {
            return;
        }

        if (m_options.token && m_options.token.is_cancelled()) {
            auto hold = this->shared_from_this();
            cancel();
            return;
        }

        if (m_has_deadline && std::chrono::steady_clock::now() >= m_deadline) {
            auto hold = this->shared_from_this();
            auto on_timeout = std::move(m_options.on_timeout);
            cancel();
            if (on_timeout) {
                try {
                    on_timeout();
                } catch (...) {
                    m_bus.report_exception_noexcept(std::current_exception());
                }
            }
        }
    }

    /// \brief Destroy awaiter and cancel its subscription.
    ~EventAwaiter() override {
        cancel();
    }

private:
    EventAwaiter(EventBus& bus,
                 Predicate predicate,
                 Callback callback,
                 AwaitOptions options,
                 bool single_shot,
                 std::weak_ptr<void> guard,
                 bool has_guard)
        : m_bus(bus),
          m_predicate(std::move(predicate)),
          m_callback(std::move(callback)),
          m_options(std::move(options)),
          m_single_shot(single_shot),
          m_has_subscription_guard(has_guard),
          m_subscription_guard(std::move(guard)) {
        if (m_options.timeout.count() > 0) {
            m_has_deadline = true;
            m_deadline = std::chrono::steady_clock::now() + m_options.timeout;
        }
    }

    void subscribe_internal() {
        if (m_single_shot) {
            m_retain_self = this->shared_from_this();
        }

        auto weak_self = this->weak_from_this();
        auto callback = [weak_self](const EventType& event) {
            if (auto self = weak_self.lock()) {
                self->handle_event(event);
            }
        };

        if (m_has_subscription_guard) {
            m_subscription_id = m_bus.subscribe<EventType>(
                this,
                m_options.delivery,
                m_subscription_guard,
                std::move(callback));
        } else {
            m_subscription_id = m_bus.subscribe<EventType>(
                this,
                m_options.delivery,
                std::move(callback));
        }
    }

    void handle_event(const EventType& event) {
        if (!is_active()) {
            return;
        }

        if (m_options.token && m_options.token.is_cancelled()) {
            cancel();
            return;
        }

        auto hold = this->shared_from_this();
        const bool matched = !m_predicate || m_predicate(event);
        if (!matched) {
            return;
        }

        if (m_single_shot) {
            cancel();
        }

        if (m_callback) {
            m_callback(event);
        }
    }

private:
    EventBus& m_bus;
    EventBus::SubscriptionId m_subscription_id = 0;
    Predicate m_predicate;
    Callback m_callback;
    AwaitOptions m_options;
    bool m_single_shot = true;
    bool m_has_subscription_guard = false;
    std::weak_ptr<void> m_subscription_guard;
    bool m_has_deadline = false;
    std::chrono::steady_clock::time_point m_deadline{};
    std::atomic_bool m_cancelled{false};
    std::shared_ptr<EventAwaiter> m_retain_self;
};

} // namespace event_hub

#endif // EVENT_HUB_EVENT_AWAITER_HPP_INCLUDED
