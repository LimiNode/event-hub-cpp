#pragma once
#ifndef EVENT_HUB_EVENT_BUS_HPP_INCLUDED
#define EVENT_HUB_EVENT_BUS_HPP_INCLUDED

/// \file event_bus.hpp
/// \brief Defines the central typed event bus.

#include "awaiter_interfaces.hpp"
#include "event.hpp"
#include "event_listener.hpp"
#include "notifier.hpp"
#include "request.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace event_hub {

namespace detail {

template <typename EventType, typename... Args>
struct IsSingleEventArgument : std::false_type {};

template <typename EventType, typename Arg>
struct IsSingleEventArgument<EventType, Arg>
    : std::is_same<typename std::decay<Arg>::type, EventType> {};

} // namespace detail

template <typename EventType>
class EventAwaiter;

/// \brief Delivery contract requested by an event subscription.
enum class DeliveryPolicy : std::uint8_t {
    direct, ///< Receive only immediate emit_direct() delivery on the caller thread.
    queued, ///< Receive only queued delivery from process() or run-loop threads.
    any ///< Receive both direct emit_direct() and queued process() delivery.
};

/// \brief Per-event dispatch statistics.
struct DispatchResult {
    std::size_t matched = 0; ///< Subscriptions for the dispatched event type.
    std::size_t delivered = 0; ///< Callbacks actually invoked.
    std::size_t skipped = 0; ///< Subscriptions skipped by DeliveryPolicy.
};

/// \brief Diagnostic payload for delivery-policy mismatches.
struct DeliveryMismatch {
    std::type_index event_type{typeid(void)}; ///< Event type being dispatched.
    DeliveryPolicy dispatch_policy = DeliveryPolicy::any; ///< Dispatch source policy.
    std::size_t skipped_subscribers = 0; ///< Subscribers skipped by DeliveryPolicy.
};

/// \brief Controls when delivery-policy mismatches are reported.
enum class DeliveryMismatchReportMode : std::uint8_t {
    no_delivery, ///< Report only when policy filtering prevents all delivery.
    any_skipped ///< Report whenever at least one subscriber is policy-skipped.
};

/// \class EventBus
/// \brief Central typed event bus with synchronous and queued dispatch.
///
/// Subscriptions are keyed by concrete C++ type. `post<T>()` is safe for
/// producer threads; callbacks are invoked on the thread that calls
/// `emit_direct<T>()` or `process()` according to the subscription
/// DeliveryPolicy. Guarded
/// subscriptions are skipped when their guard expires before the callback
/// starts.
///
/// The bus copies matching callback records before dispatch and releases its
/// subscription mutex before invoking user code. Handlers may therefore post,
/// subscribe, unsubscribe, or cancel awaiters while an event is being handled.
///
/// \note EventBus must outlive all EventEndpoint and EventAwaiter instances
/// that reference it.
class EventBus {
public:
    using SubscriptionId = std::uint64_t; ///< Unique subscription identifier.
    using ExceptionHandler = std::function<void(std::exception_ptr)>; ///< Callback for user callback exceptions.
    using DeliveryMismatchHandler = std::function<void(const DeliveryMismatch&)>; ///< Callback for policy mismatch diagnostics.

    /// \brief Construct an empty event bus.
    EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    EventBus(EventBus&&) = delete;
    EventBus& operator=(EventBus&&) = delete;

    /// \brief Set a non-owning notifier called after events are queued.
    ///
    /// The caller must keep the notifier alive while producer threads may call
    /// post(), or call reset_notifier() before destroying it.
    ///
    /// \param notifier Non-owning notifier pointer, or null to disable
    /// notifications.
    void set_notifier(INotifier* notifier) noexcept {
        m_notifier.store(notifier, std::memory_order_release);
    }

    /// \brief Remove the currently configured notifier.
    void reset_notifier() noexcept {
        m_notifier.store(nullptr, std::memory_order_release);
    }

    /// \brief Set a handler for exceptions thrown by user callbacks.
    ///
    /// When a handler is set, dispatch reports callback exceptions to it and
    /// continues. Without a handler, callback exceptions are rethrown.
    ///
    /// \param handler Handler to install. Passing an empty handler restores
    /// fail-fast dispatch behavior.
    void set_exception_handler(ExceptionHandler handler) {
        std::lock_guard<std::mutex> lock(m_exception_handler_mutex);
        m_exception_handler = std::move(handler);
    }

    /// \brief Set a handler for delivery-policy mismatches.
    /// \param handler Handler to install. Passing an empty handler disables
    /// mismatch diagnostics.
    /// \param mode Controls which policy skips are reported.
    ///
    /// By default the handler is called only when subscribers for the event
    /// type exist but no callback was delivered because DeliveryPolicy filtered
    /// them out. Use DeliveryMismatchReportMode::any_skipped for verbose
    /// diagnostics on mixed direct/queued subscriptions. Handler exceptions are
    /// reported to the bus exception handler when one is configured; otherwise
    /// they are ignored.
    void set_delivery_mismatch_handler(
        DeliveryMismatchHandler handler,
        DeliveryMismatchReportMode mode = DeliveryMismatchReportMode::no_delivery) {
        std::lock_guard<std::mutex> lock(m_delivery_mismatch_handler_mutex);
        m_delivery_mismatch_handler = std::move(handler);
        m_delivery_mismatch_report_mode = mode;
    }

    /// \brief Return a bus-wide request id for request-response event pairs.
    /// \return Generated request id that is unique for this EventBus instance.
    RequestId next_request_id() noexcept {
        return m_request_ids.next();
    }

    /// \brief Subscribe an owner to both direct and queued delivery.
    /// \tparam EventType Concrete event type to receive.
    /// \tparam Callback Callback type invocable with `const EventType&`.
    /// \param owner Non-owning owner key used for grouped unsubscription.
    /// \param callback Callback invoked when EventType is dispatched.
    /// \return Subscription id that can be used for targeted unsubscription.
    template <typename EventType,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    SubscriptionId subscribe_any(void* owner, Callback&& callback) {
        return subscribe<EventType>(
            owner,
            DeliveryPolicy::any,
            std::forward<Callback>(callback));
    }

    /// \brief Subscribe an owner to a concrete event type with a delivery policy.
    /// \tparam EventType Concrete event type to receive.
    /// \tparam Callback Callback type invocable with `const EventType&`.
    /// \param owner Non-owning owner key used for grouped unsubscription.
    /// \param delivery Delivery source accepted by the subscription.
    /// \param callback Callback invoked when EventType is dispatched.
    /// \return Subscription id that can be used for targeted unsubscription.
    template <typename EventType,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    SubscriptionId subscribe(void* owner,
                             DeliveryPolicy delivery,
                             Callback&& callback) {
        static_assert(!std::is_reference<EventType>::value,
                      "EventType must not be a reference");

        std::function<void(const EventType&)> typed_callback(
            std::forward<Callback>(callback));

        const auto id = next_subscription_id();

        CallbackRecord record;
        record.id = id;
        record.owner = owner;
        record.delivery = delivery;
        record.callback = [callback = std::move(typed_callback)](
                              const void* event) {
            callback(*static_cast<const EventType*>(event));
        };

        std::lock_guard<std::mutex> lock(m_subscriptions_mutex);
        m_callbacks[std::type_index(typeid(EventType))].push_back(
            std::move(record));
        return id;
    }

    /// \brief Subscribe to both delivery sources with a lifetime guard.
    /// \tparam EventType Concrete event type to receive.
    /// \tparam Guard Type stored by the weak lifetime guard.
    /// \tparam Callback Callback type invocable with `const EventType&`.
    /// \param owner Non-owning owner key used for grouped unsubscription.
    /// \param guard Weak guard that must lock before callback invocation.
    /// \param callback Callback invoked when EventType is dispatched.
    /// \return Subscription id that can be used for targeted unsubscription.
    ///
    /// When dispatch reaches this subscription, the guard is locked and the
    /// resulting shared owner is held until the callback returns. Expired
    /// guards skip the callback.
    template <typename EventType,
              typename Guard,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    SubscriptionId subscribe_any(void* owner,
                                 std::weak_ptr<Guard> guard,
                                 Callback&& callback) {
        return subscribe<EventType>(
            owner,
            DeliveryPolicy::any,
            std::move(guard),
            std::forward<Callback>(callback));
    }

    /// \brief Subscribe with a delivery policy and lifetime guard.
    /// \tparam EventType Concrete event type to receive.
    /// \tparam Guard Type stored by the weak lifetime guard.
    /// \tparam Callback Callback type invocable with `const EventType&`.
    /// \param owner Non-owning owner key used for grouped unsubscription.
    /// \param delivery Delivery source accepted by the subscription.
    /// \param guard Weak guard that must lock before callback invocation.
    /// \param callback Callback invoked when EventType is dispatched.
    /// \return Subscription id that can be used for targeted unsubscription.
    ///
    /// When dispatch reaches this subscription, the guard is locked and the
    /// resulting shared owner is held until the callback returns. Expired
    /// guards skip the callback.
    template <typename EventType,
              typename Guard,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    SubscriptionId subscribe(void* owner,
                             DeliveryPolicy delivery,
                             std::weak_ptr<Guard> guard,
                             Callback&& callback) {
        static_assert(!std::is_reference<EventType>::value,
                      "EventType must not be a reference");

        std::function<void(const EventType&)> typed_callback(
            std::forward<Callback>(callback));

        const auto id = next_subscription_id();

        CallbackRecord record;
        record.id = id;
        record.owner = owner;
        record.delivery = delivery;
        record.has_guard = true;
        record.guard = std::weak_ptr<void>(guard);
        record.callback = [callback = std::move(typed_callback)](
                              const void* event) {
            callback(*static_cast<const EventType*>(event));
        };

        std::lock_guard<std::mutex> lock(m_subscriptions_mutex);
        m_callbacks[std::type_index(typeid(EventType))].push_back(
            std::move(record));
        return id;
    }

    /// \brief Subscribe a generic EventListener to both delivery sources.
    /// \tparam EventType Event type derived from event_hub::Event.
    /// \param owner Non-owning owner key used for grouped unsubscription.
    /// \param listener Listener whose on_event() method is invoked.
    /// \return Subscription id that can be used for targeted unsubscription.
    template <typename EventType>
    SubscriptionId subscribe_any(void* owner, EventListener& listener) {
        return subscribe<EventType>(owner, DeliveryPolicy::any, listener);
    }

    /// \brief Subscribe a generic EventListener with a delivery policy.
    /// \tparam EventType Event type derived from event_hub::Event.
    /// \param owner Non-owning owner key used for grouped unsubscription.
    /// \param delivery Delivery source accepted by the subscription.
    /// \param listener Listener whose on_event() method is invoked.
    /// \return Subscription id that can be used for targeted unsubscription.
    template <typename EventType>
    SubscriptionId subscribe(void* owner,
                             DeliveryPolicy delivery,
                             EventListener& listener) {
        static_assert(std::is_base_of<Event, EventType>::value,
                      "EventType must derive from event_hub::Event");

        return subscribe<EventType>(
            owner,
            delivery,
            [&listener](const EventType& event) {
                listener.on_event(event);
            });
    }

    /// \brief Subscribe a generic EventListener to both sources with a guard.
    /// \tparam EventType Event type derived from event_hub::Event.
    /// \tparam Guard Type stored by the weak lifetime guard.
    /// \param owner Non-owning owner key used for grouped unsubscription.
    /// \param guard Weak guard that must lock before listener invocation.
    /// \param listener Listener whose on_event() method is invoked.
    /// \return Subscription id that can be used for targeted unsubscription.
    template <typename EventType, typename Guard>
    SubscriptionId subscribe_any(void* owner,
                                 std::weak_ptr<Guard> guard,
                                 EventListener& listener) {
        return subscribe<EventType>(
            owner,
            DeliveryPolicy::any,
            std::move(guard),
            listener);
    }

    /// \brief Subscribe a generic EventListener with a delivery policy and guard.
    /// \tparam EventType Event type derived from event_hub::Event.
    /// \tparam Guard Type stored by the weak lifetime guard.
    /// \param owner Non-owning owner key used for grouped unsubscription.
    /// \param delivery Delivery source accepted by the subscription.
    /// \param guard Weak guard that must lock before listener invocation.
    /// \param listener Listener whose on_event() method is invoked.
    /// \return Subscription id that can be used for targeted unsubscription.
    template <typename EventType, typename Guard>
    SubscriptionId subscribe(void* owner,
                             DeliveryPolicy delivery,
                             std::weak_ptr<Guard> guard,
                             EventListener& listener) {
        static_assert(std::is_base_of<Event, EventType>::value,
                      "EventType must derive from event_hub::Event");

        return subscribe<EventType>(
            owner,
            delivery,
            std::move(guard),
            [&listener](const EventType& event) {
                listener.on_event(event);
            });
    }

    /// \brief Remove one subscription by id.
    /// \param id Subscription id returned by subscribe().
    ///
    /// Removing a subscription prevents future dispatch records from being
    /// copied, but does not wait for callbacks already copied by an active
    /// dispatch.
    void unsubscribe(SubscriptionId id) {
        std::lock_guard<std::mutex> lock(m_subscriptions_mutex);
        for (auto it = m_callbacks.begin(); it != m_callbacks.end();) {
            auto& callbacks = it->second;
            callbacks.erase(std::remove_if(callbacks.begin(), callbacks.end(),
                                           [id](const CallbackRecord& record) {
                                               return record.id == id;
                                           }),
                            callbacks.end());
            if (callbacks.empty()) {
                it = m_callbacks.erase(it);
            } else {
                ++it;
            }
        }
    }

    /// \brief Remove all subscriptions for an owner and concrete event type.
    /// \tparam EventType Concrete event type to unsubscribe.
    /// \param owner Non-owning owner key used when the subscriptions were
    /// created.
    template <typename EventType>
    void unsubscribe_all(void* owner) {
        std::lock_guard<std::mutex> lock(m_subscriptions_mutex);
        auto it = m_callbacks.find(std::type_index(typeid(EventType)));
        if (it == m_callbacks.end()) {
            return;
        }

        auto& callbacks = it->second;
        callbacks.erase(std::remove_if(callbacks.begin(), callbacks.end(),
                                       [owner](const CallbackRecord& record) {
                                           return record.owner == owner;
                                       }),
                        callbacks.end());
        if (callbacks.empty()) {
            m_callbacks.erase(it);
        }
    }

    /// \brief Remove all subscriptions owned by an owner.
    /// \param owner Non-owning owner key used when the subscriptions were
    /// created.
    ///
    /// This removes future records from the bus storage, but it does not stop
    /// or wait for callbacks that already started or already passed their guard
    /// check.
    void unsubscribe_all(void* owner) {
        std::lock_guard<std::mutex> lock(m_subscriptions_mutex);
        for (auto it = m_callbacks.begin(); it != m_callbacks.end();) {
            auto& callbacks = it->second;
            callbacks.erase(std::remove_if(callbacks.begin(), callbacks.end(),
                                           [owner](const CallbackRecord& record) {
                                               return record.owner == owner;
                                           }),
                            callbacks.end());
            if (callbacks.empty()) {
                it = m_callbacks.erase(it);
            } else {
                ++it;
            }
        }
    }

    /// \brief Dispatch an already constructed event synchronously.
    /// \tparam EventType Concrete event type to dispatch.
    /// \param event Event object to dispatch.
    /// \return Dispatch statistics for this event.
    ///
    /// Invokes DeliveryPolicy::direct and DeliveryPolicy::any subscriptions on
    /// the caller thread. Queued-only subscriptions are not invoked.
    ///
    /// \throws Any callback exception when no exception handler is configured.
    template <typename EventType>
    DispatchResult emit_direct(const EventType& event) const {
        const auto result = dispatch(std::type_index(typeid(EventType)),
                                     &event,
                                     DispatchSource::direct);
        poll_awaiters(DispatchSource::direct);
        return result;
    }

    /// \brief Construct and dispatch an event synchronously.
    /// \tparam EventType Concrete event type to construct and dispatch.
    /// \tparam Args Constructor argument types.
    /// \param args Arguments used to construct the event.
    /// \return Dispatch statistics for this event.
    ///
    /// Invokes DeliveryPolicy::direct and DeliveryPolicy::any subscriptions on
    /// the caller thread. Queued-only subscriptions are not invoked.
    ///
    /// \throws Any callback exception when no exception handler is configured.
    template <typename EventType,
              typename... Args,
              typename std::enable_if<
                  !detail::IsSingleEventArgument<EventType, Args...>::value,
                  int>::type = 0>
    DispatchResult emit_direct(Args&&... args) const {
        EventType event{std::forward<Args>(args)...};
        const auto result = dispatch(std::type_index(typeid(EventType)),
                                     &event,
                                     DispatchSource::direct);
        poll_awaiters(DispatchSource::direct);
        return result;
    }

    /// \brief Queue an already constructed event for later processing.
    /// \tparam EventType Concrete event type to queue.
    /// \param event Event object to copy into the queue.
    ///
    /// The later process() call invokes DeliveryPolicy::queued and
    /// DeliveryPolicy::any subscriptions on the processing thread.
    template <typename EventType>
    void post(const EventType& event) {
        enqueue<EventType>(std::make_shared<EventType>(event));
    }

    /// \brief Queue an already constructed event for later processing.
    /// \tparam EventType Concrete event type to queue.
    /// \param event Event object to move into the queue.
    ///
    /// The later process() call invokes DeliveryPolicy::queued and
    /// DeliveryPolicy::any subscriptions on the processing thread.
    template <typename EventType,
              typename std::enable_if<
                  !std::is_lvalue_reference<EventType>::value,
                  int>::type = 0>
    void post(EventType&& event) {
        using StoredEvent = typename std::decay<EventType>::type;
        enqueue<StoredEvent>(
            std::make_shared<StoredEvent>(std::forward<EventType>(event)));
    }

    /// \brief Construct and queue an event for later processing.
    /// \tparam EventType Concrete event type to construct and queue.
    /// \tparam Args Constructor argument types.
    /// \param args Arguments used to construct the queued event.
    ///
    /// The later process() call invokes DeliveryPolicy::queued and
    /// DeliveryPolicy::any subscriptions on the processing thread.
    template <typename EventType,
              typename... Args,
              typename std::enable_if<
                  !detail::IsSingleEventArgument<EventType, Args...>::value,
                  int>::type = 0>
    void post(Args&&... args) {
        enqueue<EventType>(
            std::make_shared<EventType>(EventType{std::forward<Args>(args)...}));
    }

    /// \brief Drain queued events captured at the start of the call.
    /// \details Events posted during process() are processed by a later call.
    /// If an unhandled callback exception stops processing, events not yet
    /// dispatched from the captured snapshot are restored for a later
    /// process() call.
    /// \return Number of events dispatched.
    /// \throws Any callback exception when no exception handler is configured.
    std::size_t process() {
        std::queue<QueuedEvent> local_queue;
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            std::swap(local_queue, m_event_queue);
        }

        std::size_t processed = 0;
        while (!local_queue.empty()) {
            const auto& queued = local_queue.front();
            try {
                dispatch(queued.type, queued.payload.get(), DispatchSource::queued);
                local_queue.pop();
            } catch (...) {
                local_queue.pop();
                restore_unprocessed_events(local_queue);
                throw;
            }
            ++processed;
        }

        poll_awaiters(DispatchSource::queued);
        return processed;
    }

    /// \brief Return the current queued event count.
    /// \return Number of events waiting in the async queue.
    std::size_t pending_count() const {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        return m_event_queue.size();
    }

    /// \brief Return true when the async queue is not empty.
    /// \return True when one or more queued events are pending.
    bool has_pending() const {
        return pending_count() != 0U;
    }

    /// \brief Return the earliest active awaiter timeout deadline, if any.
    ///
    /// Awaiters are passive and are polled by process()/emit_direct(). A
    /// blocking loop can use this deadline to wake and invoke process() when
    /// no event has been queued.
    std::optional<std::chrono::steady_clock::time_point>
    next_awaiter_deadline() const noexcept {
        std::optional<std::chrono::steady_clock::time_point> best;
        std::lock_guard<std::mutex> lock(m_awaiters_mutex);
        for (const auto& weak : m_awaiters) {
            if (auto awaiter = weak.lock()) {
                const auto deadline = awaiter->next_deadline();
                if (deadline && (!best || *deadline < *best)) {
                    best = deadline;
                }
            }
        }
        return best;
    }

    /// \brief Drop queued events without dispatching them.
    ///
    /// This clears only the async queue. Subscriptions and awaiters are left
    /// unchanged.
    void clear_pending() {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        std::queue<QueuedEvent> empty;
        std::swap(m_event_queue, empty);
    }

    /// \brief Drop queued events without dispatching them.
    /// \note Compatibility wrapper; prefer clear_pending() for clarity.
    void clear() {
        clear_pending();
    }

    /// \brief Register an awaiter for timeout and cancellation polling.
    /// \param awaiter Awaiter implementation to poll at emit_direct() and
    /// process() points accepted by its delivery policy. Null pointers are
    /// ignored.
    void register_awaiter(const std::shared_ptr<IAwaiterEx>& awaiter) {
        if (!awaiter) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_awaiters_mutex);
        m_awaiters.emplace_back(awaiter);
    }

private:
    struct CallbackRecord {
        SubscriptionId id = 0;
        void* owner = nullptr;
        DeliveryPolicy delivery = DeliveryPolicy::any;
        bool has_guard = false;
        std::weak_ptr<void> guard;
        std::function<void(const void*)> callback;
    };

    struct QueuedEvent {
        std::type_index type;
        std::shared_ptr<const void> payload;
    };

    struct DeliveryMismatchHandlerSnapshot {
        DeliveryMismatchHandler handler;
        DeliveryMismatchReportMode mode = DeliveryMismatchReportMode::no_delivery;
    };

    template <typename EventType>
    void enqueue(std::shared_ptr<EventType> event) {
        std::shared_ptr<const void> payload = std::move(event);
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            m_event_queue.push(QueuedEvent{std::type_index(typeid(EventType)),
                                           std::move(payload)});
        }

        notify_work_available();
    }

    void notify_work_available() noexcept {
        auto* notifier = m_notifier.load(std::memory_order_acquire);
        if (notifier) {
            notifier->notify();
        }
    }

    void restore_unprocessed_events(std::queue<QueuedEvent>& remaining) {
        if (remaining.empty()) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_queue_mutex);
        if (m_event_queue.empty()) {
            std::swap(m_event_queue, remaining);
            return;
        }

        std::queue<QueuedEvent> restored;
        while (!remaining.empty()) {
            restored.push(std::move(remaining.front()));
            remaining.pop();
        }
        while (!m_event_queue.empty()) {
            restored.push(std::move(m_event_queue.front()));
            m_event_queue.pop();
        }
        std::swap(m_event_queue, restored);
    }

    static bool accepts_delivery(DeliveryPolicy delivery,
                                 DispatchSource source) noexcept {
        switch (delivery) {
        case DeliveryPolicy::direct:
            return source == DispatchSource::direct;
        case DeliveryPolicy::queued:
            return source == DispatchSource::queued;
        case DeliveryPolicy::any:
            return true;
        }
        return false;
    }

    static DeliveryPolicy source_policy(DispatchSource source) noexcept {
        return source == DispatchSource::direct ? DeliveryPolicy::direct
                                                : DeliveryPolicy::queued;
    }

    void report_delivery_mismatch(std::type_index type,
                                  DispatchSource source,
                                  const DispatchResult& result) const noexcept {
        if (result.skipped == 0U) {
            return;
        }

        auto snapshot = delivery_mismatch_handler_snapshot();
        if (!snapshot.handler) {
            return;
        }

        if (snapshot.mode == DeliveryMismatchReportMode::no_delivery &&
            result.delivered != 0U) {
            return;
        }

        try {
            snapshot.handler(
                DeliveryMismatch{type, source_policy(source), result.skipped});
        } catch (...) {
            report_exception_noexcept(std::current_exception());
        }
    }

    DispatchResult dispatch(std::type_index type,
                            const void* event,
                            DispatchSource source) const {
        std::vector<CallbackRecord> callbacks;
        {
            std::lock_guard<std::mutex> lock(m_subscriptions_mutex);
            auto it = m_callbacks.find(type);
            if (it != m_callbacks.end()) {
                callbacks = it->second;
            }
        }

        DispatchResult result;
        result.matched = callbacks.size();

        for (const auto& record : callbacks) {
            if (!accepts_delivery(record.delivery, source)) {
                ++result.skipped;
                continue;
            }

            std::shared_ptr<void> alive;
            if (record.has_guard) {
                alive = record.guard.lock();
                if (!alive) {
                    continue;
                }
            }

            if (record.callback) {
                try {
                    ++result.delivered;
                    record.callback(event);
                } catch (...) {
                    if (!report_exception(std::current_exception())) {
                        throw;
                    }
                }
            }
        }

        report_delivery_mismatch(type, source, result);
        return result;
    }

    SubscriptionId next_subscription_id() {
        return m_next_subscription_id.fetch_add(1, std::memory_order_relaxed);
    }

    ExceptionHandler exception_handler() const {
        std::lock_guard<std::mutex> lock(m_exception_handler_mutex);
        return m_exception_handler;
    }

    DeliveryMismatchHandlerSnapshot delivery_mismatch_handler_snapshot() const {
        std::lock_guard<std::mutex> lock(m_delivery_mismatch_handler_mutex);
        return DeliveryMismatchHandlerSnapshot{m_delivery_mismatch_handler,
                                               m_delivery_mismatch_report_mode};
    }

    bool report_exception(std::exception_ptr exception) const {
        auto handler = exception_handler();
        if (!handler) {
            return false;
        }

        handler(std::move(exception));
        return true;
    }

    void report_exception_noexcept(std::exception_ptr exception) const noexcept {
        try {
            (void)report_exception(std::move(exception));
        } catch (...) {
        }
    }

    void poll_awaiters(DispatchSource source) const {
        std::vector<std::shared_ptr<IAwaiterEx>> live;
        {
            std::lock_guard<std::mutex> lock(m_awaiters_mutex);
            m_awaiters.erase(
                std::remove_if(m_awaiters.begin(), m_awaiters.end(),
                               [](const std::weak_ptr<IAwaiterEx>& weak) {
                                   auto awaiter = weak.lock();
                                   return !awaiter || !awaiter->is_active();
                               }),
                m_awaiters.end());

            live.reserve(m_awaiters.size());
            for (const auto& weak : m_awaiters) {
                if (auto awaiter = weak.lock()) {
                    live.emplace_back(std::move(awaiter));
                }
            }
        }

        for (auto& awaiter : live) {
            awaiter->poll_timeout(source);
        }
    }

private:
    template <typename EventType>
    friend class EventAwaiter;

    mutable std::mutex m_subscriptions_mutex;
    std::unordered_map<std::type_index, std::vector<CallbackRecord>> m_callbacks;

    mutable std::mutex m_queue_mutex;
    std::queue<QueuedEvent> m_event_queue;

    mutable std::mutex m_awaiters_mutex;
    mutable std::vector<std::weak_ptr<IAwaiterEx>> m_awaiters;

    mutable std::mutex m_exception_handler_mutex;
    ExceptionHandler m_exception_handler;

    mutable std::mutex m_delivery_mismatch_handler_mutex;
    DeliveryMismatchHandler m_delivery_mismatch_handler;
    DeliveryMismatchReportMode m_delivery_mismatch_report_mode =
        DeliveryMismatchReportMode::no_delivery;

    std::atomic<SubscriptionId> m_next_subscription_id{1};
    RequestIdGenerator m_request_ids;
    std::atomic<INotifier*> m_notifier{nullptr};
};

} // namespace event_hub

#endif // EVENT_HUB_EVENT_BUS_HPP_INCLUDED
