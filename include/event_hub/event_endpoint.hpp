#pragma once
#ifndef EVENT_HUB_EVENT_ENDPOINT_HPP_INCLUDED
#define EVENT_HUB_EVENT_ENDPOINT_HPP_INCLUDED

/// \file event_endpoint.hpp
/// \brief Defines the RAII module endpoint for EventBus.

#include "event_awaiter.hpp"
#include "event_bus.hpp"
#include "request.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace event_hub {

/// \class EventEndpointClosedError
/// \brief Exception stored in request_future() when the endpoint is closed.
class EventEndpointClosedError : public std::runtime_error {
public:
    EventEndpointClosedError()
        : std::runtime_error("event_hub endpoint is closed") {}
};

/// \class EventEndpoint
/// \brief RAII connection point that owns subscriptions and awaiters.
///
/// An endpoint is usually owned by one module. Destroying it cancels awaiters
/// and removes all subscriptions registered through the endpoint. Endpoint
/// subscriptions carry a lifetime guard so callbacks copied by an active
/// dispatch are skipped if the endpoint guard has already expired.
/// subscribe() requires an explicit DeliveryPolicy. subscribe_direct()
/// callbacks run only from emit_direct() caller threads. subscribe_queued()
/// callbacks run only from process() or run-loop threads. subscribe_any() keeps the
/// compatibility behavior and accepts both sources.
///
/// Use the overloads that take `std::weak_ptr` when callbacks also touch an
/// object whose lifetime is separate from the endpoint. In that case both the
/// endpoint guard and the user guard must still be alive before user code runs.
///
/// \note EventBus must outlive every EventEndpoint connected to it.
class EventEndpoint {
public:
    /// \brief Construct an endpoint connected to a bus.
    /// \param bus Event bus that must outlive this endpoint.
    explicit EventEndpoint(EventBus& bus)
        : m_bus(bus),
          m_guard(std::make_shared<int>(0)) {}

    EventEndpoint(const EventEndpoint&) = delete;
    EventEndpoint& operator=(const EventEndpoint&) = delete;
    EventEndpoint(EventEndpoint&&) = delete;
    EventEndpoint& operator=(EventEndpoint&&) = delete;

    /// \brief Cancel awaiters and remove subscriptions owned by this endpoint.
    ~EventEndpoint() {
        close();
    }

    /// \brief Cancel awaiters and remove subscriptions owned by this endpoint.
    ///
    /// Closing is idempotent. It expires the endpoint lifetime guard, cancels
    /// awaiters created through the endpoint, and removes endpoint-owned
    /// subscriptions from the bus.
    ///
    /// Callbacks that have already started, or that already passed their guard
    /// check, are not stopped or waited for.
    void close() {
        if (!m_closed) {
            m_closed = true;
            m_guard.reset();
        }

        cancel_awaiters();
        // Bypass the public closed-state guard so close() can always clean up
        // subscriptions, including when called repeatedly.
        m_bus.unsubscribe_all(this);
    }

    /// \brief Return true after this endpoint has been explicitly closed.
    /// \return True after close() has been called.
    bool is_closed() const noexcept {
        return m_closed;
    }

    /// \brief Return the bus referenced by this endpoint.
    /// \return Mutable event bus reference.
    EventBus& bus() noexcept {
        return m_bus;
    }

    /// \brief Return the bus referenced by this endpoint.
    /// \return Const event bus reference.
    const EventBus& bus() const noexcept {
        return m_bus;
    }

    /// \brief Subscribe this endpoint to both delivery sources.
    /// \tparam EventType Concrete event type to receive.
    /// \tparam Callback Callback type invocable with `const EventType&`.
    /// \param callback Callback invoked when EventType is dispatched.
    /// \return Subscription id that can be used for targeted unsubscription.
    ///
    /// The subscription uses the endpoint lifetime guard. Closing or
    /// destroying the endpoint prevents guarded callbacks from starting later.
    template <typename EventType,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    EventBus::SubscriptionId subscribe_any(Callback&& callback) {
        return subscribe<EventType>(
            DeliveryPolicy::any,
            std::forward<Callback>(callback));
    }

    /// \brief Subscribe this endpoint with an explicit delivery policy.
    /// \tparam EventType Concrete event type to receive.
    /// \tparam Callback Callback type invocable with `const EventType&`.
    /// \param delivery Delivery source accepted by the subscription.
    /// \param callback Callback invoked when EventType is dispatched.
    /// \return Subscription id that can be used for targeted unsubscription.
    template <typename EventType,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    EventBus::SubscriptionId subscribe(DeliveryPolicy delivery,
                                       Callback&& callback) {
        if (m_closed) {
            return 0U;
        }
        return m_bus.subscribe<EventType>(
            this,
            delivery,
            guard(),
            std::forward<Callback>(callback));
    }

    /// \brief Subscribe to direct emit_direct() delivery only.
    template <typename EventType,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    EventBus::SubscriptionId subscribe_direct(Callback&& callback) {
        return subscribe<EventType>(
            DeliveryPolicy::direct,
            std::forward<Callback>(callback));
    }

    /// \brief Subscribe to queued process() delivery only.
    template <typename EventType,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    EventBus::SubscriptionId subscribe_queued(Callback&& callback) {
        return subscribe<EventType>(
            DeliveryPolicy::queued,
            std::forward<Callback>(callback));
    }

    /// \brief Subscribe to both delivery sources with an additional user guard.
    /// \tparam EventType Concrete event type to receive.
    /// \tparam Guard Type stored by the user weak guard.
    /// \tparam Callback Callback type invocable with `const EventType&`.
    /// \param user_guard Additional weak guard that must lock before the
    /// callback is invoked.
    /// \param callback Callback invoked when EventType is dispatched.
    /// \return Subscription id that can be used for targeted unsubscription.
    ///
    /// The bus record still uses the endpoint guard. The user guard is checked
    /// inside the endpoint-owned callback, so both guards must be valid before
    /// user code runs.
    template <typename EventType,
              typename Guard,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    EventBus::SubscriptionId subscribe_any(std::weak_ptr<Guard> user_guard,
                                           Callback&& callback) {
        return subscribe<EventType>(
            DeliveryPolicy::any,
            std::move(user_guard),
            std::forward<Callback>(callback));
    }

    /// \brief Subscribe with a delivery policy and additional lifetime guard.
    /// \tparam EventType Concrete event type to receive.
    /// \tparam Guard Type stored by the user weak guard.
    /// \tparam Callback Callback type invocable with `const EventType&`.
    /// \param delivery Delivery source accepted by the subscription.
    /// \param user_guard Additional weak guard that must lock before the
    /// callback is invoked.
    /// \param callback Callback invoked when EventType is dispatched.
    /// \return Subscription id that can be used for targeted unsubscription.
    template <typename EventType,
              typename Guard,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    EventBus::SubscriptionId subscribe(DeliveryPolicy delivery,
                                       std::weak_ptr<Guard> user_guard,
                                       Callback&& callback) {
        if (m_closed) {
            return 0U;
        }
        std::function<void(const EventType&)> typed_callback(
            std::forward<Callback>(callback));

        return m_bus.subscribe<EventType>(
            this,
            delivery,
            guard(),
            [user_guard = std::move(user_guard),
             callback = std::move(typed_callback)](const EventType& event) mutable {
                auto alive = user_guard.lock();
                if (!alive) {
                    return;
                }

                callback(event);
            });
    }

    /// \brief Subscribe to direct emit_direct() delivery with an additional guard.
    template <typename EventType,
              typename Guard,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    EventBus::SubscriptionId subscribe_direct(std::weak_ptr<Guard> user_guard,
                                              Callback&& callback) {
        return subscribe<EventType>(
            DeliveryPolicy::direct,
            std::move(user_guard),
            std::forward<Callback>(callback));
    }

    /// \brief Subscribe to queued process() delivery with an additional guard.
    template <typename EventType,
              typename Guard,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    EventBus::SubscriptionId subscribe_queued(std::weak_ptr<Guard> user_guard,
                                              Callback&& callback) {
        return subscribe<EventType>(
            DeliveryPolicy::queued,
            std::move(user_guard),
            std::forward<Callback>(callback));
    }

    /// \brief Subscribe an EventListener to both delivery sources.
    /// \tparam EventType Event type derived from event_hub::Event.
    /// \param listener Listener whose on_event() method is invoked.
    /// \return Subscription id that can be used for targeted unsubscription.
    template <typename EventType>
    EventBus::SubscriptionId subscribe_any(EventListener& listener) {
        return subscribe<EventType>(DeliveryPolicy::any, listener);
    }

    /// \brief Subscribe an EventListener with an explicit delivery policy.
    /// \tparam EventType Event type derived from event_hub::Event.
    /// \param delivery Delivery source accepted by the subscription.
    /// \param listener Listener whose on_event() method is invoked.
    /// \return Subscription id that can be used for targeted unsubscription.
    template <typename EventType>
    EventBus::SubscriptionId subscribe(DeliveryPolicy delivery,
                                       EventListener& listener) {
        if (m_closed) {
            return 0U;
        }
        return m_bus.subscribe<EventType>(this, delivery, guard(), listener);
    }

    /// \brief Subscribe an EventListener to direct emit_direct() delivery only.
    template <typename EventType>
    EventBus::SubscriptionId subscribe_direct(EventListener& listener) {
        return subscribe<EventType>(DeliveryPolicy::direct, listener);
    }

    /// \brief Subscribe an EventListener to queued process() delivery only.
    template <typename EventType>
    EventBus::SubscriptionId subscribe_queued(EventListener& listener) {
        return subscribe<EventType>(DeliveryPolicy::queued, listener);
    }

    /// \brief Subscribe an EventListener to both sources with a user guard.
    /// \tparam EventType Event type derived from event_hub::Event.
    /// \tparam Guard Type stored by the user weak guard.
    /// \param user_guard Additional weak guard that must lock before listener
    /// invocation.
    /// \param listener Listener whose on_event() method is invoked.
    /// \return Subscription id that can be used for targeted unsubscription.
    template <typename EventType, typename Guard>
    EventBus::SubscriptionId subscribe_any(std::weak_ptr<Guard> user_guard,
                                           EventListener& listener) {
        return subscribe<EventType>(
            DeliveryPolicy::any,
            std::move(user_guard),
            listener);
    }

    /// \brief Subscribe an EventListener with delivery policy and user guard.
    /// \tparam EventType Event type derived from event_hub::Event.
    /// \tparam Guard Type stored by the user weak guard.
    /// \param delivery Delivery source accepted by the subscription.
    /// \param user_guard Additional weak guard that must lock before listener
    /// invocation.
    /// \param listener Listener whose on_event() method is invoked.
    /// \return Subscription id that can be used for targeted unsubscription.
    template <typename EventType, typename Guard>
    EventBus::SubscriptionId subscribe(DeliveryPolicy delivery,
                                       std::weak_ptr<Guard> user_guard,
                                       EventListener& listener) {
        return subscribe<EventType>(
            delivery,
            std::move(user_guard),
            [&listener](const EventType& event) {
                listener.on_event(event);
            });
    }

    /// \brief Subscribe an EventListener to direct emit_direct() delivery with a guard.
    template <typename EventType, typename Guard>
    EventBus::SubscriptionId subscribe_direct(std::weak_ptr<Guard> user_guard,
                                              EventListener& listener) {
        return subscribe<EventType>(
            DeliveryPolicy::direct,
            std::move(user_guard),
            listener);
    }

    /// \brief Subscribe an EventListener to queued process() delivery with a guard.
    template <typename EventType, typename Guard>
    EventBus::SubscriptionId subscribe_queued(std::weak_ptr<Guard> user_guard,
                                              EventListener& listener) {
        return subscribe<EventType>(
            DeliveryPolicy::queued,
            std::move(user_guard),
            listener);
    }

    /// \brief Remove all subscriptions for a concrete event type.
    /// \tparam EventType Concrete event type to unsubscribe.
    template <typename EventType>
    void unsubscribe() {
        if (m_closed) {
            return;
        }
        m_bus.template unsubscribe_all<EventType>(this);
    }

    /// \brief Remove one subscription by id.
    /// \param id Subscription id returned by subscribe().
    void unsubscribe(EventBus::SubscriptionId id) {
        if (m_closed) {
            return;
        }
        m_bus.unsubscribe(id);
    }

    /// \brief Remove all subscriptions owned by this endpoint.
    ///
    /// This removes future records from the bus storage, but it does not wait
    /// for callbacks that already started.
    void unsubscribe_all() {
        if (m_closed) {
            return;
        }
        m_bus.unsubscribe_all(this);
    }

    /// \brief Dispatch an already constructed event synchronously.
    /// \tparam EventType Concrete event type to dispatch.
    /// \param event Event object to dispatch.
    /// \return Dispatch statistics for this event.
    /// \throws Any callback exception when no exception handler is configured
    /// on the bus.
    template <typename EventType>
    DispatchResult emit_direct(const EventType& event) const {
        if (m_closed) {
            return {};
        }
        return m_bus.emit_direct<EventType>(event);
    }

    /// \brief Construct and dispatch an event synchronously.
    /// \tparam EventType Concrete event type to construct and dispatch.
    /// \tparam Args Constructor argument types.
    /// \param args Arguments used to construct the event.
    /// \return Dispatch statistics for this event.
    /// \throws Any callback exception when no exception handler is configured
    /// on the bus.
    template <typename EventType,
              typename... Args,
              typename std::enable_if<
                  !detail::IsSingleEventArgument<EventType, Args...>::value,
                  int>::type = 0>
    DispatchResult emit_direct(Args&&... args) const {
        if (m_closed) {
            return {};
        }
        return m_bus.template emit_direct<EventType>(
            std::forward<Args>(args)...);
    }

    /// \brief Queue an already constructed event for later processing.
    /// \tparam EventType Concrete event type to queue.
    /// \param event Event object to copy into the queue.
    template <typename EventType>
    void post(const EventType& event) {
        if (m_closed) {
            return;
        }
        m_bus.template post<EventType>(event);
    }

    /// \brief Queue an already constructed event for later processing.
    /// \tparam EventType Concrete event type to queue.
    /// \param event Event object to move into the queue.
    template <typename EventType,
              typename std::enable_if<
                  !std::is_lvalue_reference<EventType>::value,
                  int>::type = 0>
    void post(EventType&& event) {
        if (m_closed) {
            return;
        }
        m_bus.template post<EventType>(std::forward<EventType>(event));
    }

    /// \brief Construct and queue an event for later processing.
    /// \tparam EventType Concrete event type to construct and queue.
    /// \tparam Args Constructor argument types.
    /// \param args Arguments used to construct the queued event.
    template <typename EventType,
              typename... Args,
              typename std::enable_if<
                  !detail::IsSingleEventArgument<EventType, Args...>::value,
                  int>::type = 0>
    void post(Args&&... args) {
        if (m_closed) {
            return;
        }
        m_bus.template post<EventType>(std::forward<Args>(args)...);
    }

    /// \brief Await one matching event and then auto-cancel.
    /// \tparam EventType Concrete event type to await.
    /// \tparam Predicate Predicate type invocable with `const EventType&`.
    /// \tparam Callback Callback type invocable with `const EventType&`.
    /// \param predicate Predicate that selects matching events.
    /// \param callback Callback invoked for the first matching event.
    /// \param options Timeout and cancellation options.
    /// \return Shared cancelable awaiter handle.
    ///
    /// Timeouts and cancellation tokens are checked when the bus reaches an
    /// emit_direct() or process() poll point accepted by AwaitOptions::delivery.
    template <typename EventType,
              typename Predicate,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Predicate&, const EventType&> &&
                      std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    std::shared_ptr<IAwaiter> await_once(Predicate&& predicate,
                                         Callback&& callback,
                                         AwaitOptions options = {}) {
        if (m_closed) {
            return {};
        }
        return make_awaiter<EventType>(std::forward<Predicate>(predicate),
                                       std::forward<Callback>(callback),
                                       std::move(options),
                                       true);
    }

    /// \brief Await the next event without a predicate.
    /// \tparam EventType Concrete event type to await.
    /// \tparam Callback Callback type invocable with `const EventType&`.
    /// \param callback Callback invoked for the next event of EventType.
    /// \param options Timeout and cancellation options.
    /// \return Shared cancelable awaiter handle.
    template <typename EventType,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    std::shared_ptr<IAwaiter> await_once(Callback&& callback,
                                         AwaitOptions options = {}) {
        return await_once<EventType>(
            [](const EventType&) { return true; },
            std::forward<Callback>(callback),
            std::move(options));
    }

    /// \brief Await each matching event until the returned handle is cancelled.
    /// \tparam EventType Concrete event type to await.
    /// \tparam Predicate Predicate type invocable with `const EventType&`.
    /// \tparam Callback Callback type invocable with `const EventType&`.
    /// \param predicate Predicate that selects matching events.
    /// \param callback Callback invoked for each matching event.
    /// \param options Timeout and cancellation options.
    /// \return Shared cancelable awaiter handle.
    template <typename EventType,
              typename Predicate,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Predicate&, const EventType&> &&
                      std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    std::shared_ptr<IAwaiter> await_each(Predicate&& predicate,
                                         Callback&& callback,
                                         AwaitOptions options = {}) {
        if (m_closed) {
            return {};
        }
        return make_awaiter<EventType>(std::forward<Predicate>(predicate),
                                       std::forward<Callback>(callback),
                                       std::move(options),
                                       false);
    }

    /// \brief Await every event of the type until the returned handle is cancelled.
    /// \tparam EventType Concrete event type to await.
    /// \tparam Callback Callback type invocable with `const EventType&`.
    /// \param callback Callback invoked for each event of EventType.
    /// \param options Timeout and cancellation options.
    /// \return Shared cancelable awaiter handle.
    template <typename EventType,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    std::shared_ptr<IAwaiter> await_each(Callback&& callback,
                                         AwaitOptions options = {}) {
        return await_each<EventType>(
            [](const EventType&) { return true; },
            std::forward<Callback>(callback),
            std::move(options));
    }

    /// \brief Post a request event and await the paired result event.
    /// \tparam RequestEvent Request event type.
    /// \tparam ResultEvent Result event type.
    /// \tparam Callback Callback type invocable with `const ResultEvent&`.
    /// \param request Request event. Its request id is overwritten.
    /// \param callback Callback invoked for the first result with the same id.
    /// \param options Timeout and cancellation options for the awaiter.
    /// \return Generated request id stored in the request event.
    template <typename RequestEvent,
              typename ResultEvent,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Callback&, const ResultEvent&>,
                  int>::type = 0>
    RequestId request(RequestEvent request,
                      Callback&& callback,
                      AwaitOptions options = {}) {
        if (m_closed) {
            return invalid_request_id;
        }
        const auto id = next_request_id();
        RequestTraits<RequestEvent>::set_id(request, id);

        await_once<ResultEvent>(
            [id](const ResultEvent& result) {
                return RequestTraits<ResultEvent>::get_id(result) == id;
            },
            std::forward<Callback>(callback),
            std::move(options));

        post<RequestEvent>(std::move(request));
        return id;
    }

    /// \brief Post a request event and return a future for the paired result.
    /// \tparam RequestEvent Request event type.
    /// \tparam ResultEvent Result event type.
    /// \param request Request event. Its request id is overwritten.
    /// \param options Timeout and cancellation options for the awaiter.
    /// \return Future completed by the first matching result event.
    template <typename RequestEvent, typename ResultEvent>
    std::future<ResultEvent> request_future(RequestEvent request,
                                            AwaitOptions options = {}) {
        auto promise = std::make_shared<std::promise<ResultEvent>>();
        auto future = promise->get_future();
        if (m_closed) {
            promise->set_exception(
                std::make_exception_ptr(EventEndpointClosedError()));
            return future;
        }

        auto completed = std::make_shared<std::atomic_bool>(false);
        auto request_id = std::make_shared<RequestId>(invalid_request_id);

        auto user_on_timeout = std::move(options.on_timeout);
        options.on_timeout = [promise, completed, request_id, user_on_timeout] {
            bool expected = false;
            if (completed->compare_exchange_strong(expected, true,
                                                   std::memory_order_relaxed)) {
                promise->set_exception(
                    std::make_exception_ptr(RequestTimeoutError(*request_id)));
            }
            if (user_on_timeout) {
                user_on_timeout();
            }
        };

        auto user_on_cancel = std::move(options.on_cancel);
        options.on_cancel = [promise, completed, request_id, user_on_cancel] {
            bool expected = false;
            if (completed->compare_exchange_strong(expected, true,
                                                   std::memory_order_relaxed)) {
                promise->set_exception(
                    std::make_exception_ptr(RequestCancelledError(*request_id)));
            }
            if (user_on_cancel) {
                user_on_cancel();
            }
        };

        *request_id = this->request<RequestEvent, ResultEvent>(
            std::move(request),
            [promise, completed](const ResultEvent& result) {
                bool expected = false;
                if (completed->compare_exchange_strong(expected, true,
                                                       std::memory_order_relaxed)) {
                    promise->set_value(result);
                }
            },
            std::move(options));

        return future;
    }

    /// \brief Return the next bus-wide request id.
    /// \return Generated request id.
    RequestId next_request_id() noexcept {
        if (m_closed) {
            return invalid_request_id;
        }
        return m_bus.next_request_id();
    }

    /// \brief Cancel all awaiters created by this endpoint.
    ///
    /// Cancellation is idempotent for each awaiter. This does not affect
    /// awaiters created directly through EventAwaiter::create().
    void cancel_awaiters() noexcept {
        std::vector<std::shared_ptr<IAwaiter>> awaiters;
        {
            std::lock_guard<std::mutex> lock(m_awaiters_mutex);
            awaiters.swap(m_awaiters);
        }

        for (auto& awaiter : awaiters) {
            if (awaiter) {
                awaiter->cancel();
            }
        }
    }

private:
    template <typename EventType,
              typename Predicate,
              typename Callback,
              typename std::enable_if<
                  std::is_invocable_v<Predicate&, const EventType&> &&
                      std::is_invocable_v<Callback&, const EventType&>,
                  int>::type = 0>
    std::shared_ptr<IAwaiter> make_awaiter(Predicate&& predicate,
                                           Callback&& callback,
                                           AwaitOptions options,
                                           bool single_shot) {
        prune_awaiters();

        using Awaiter = EventAwaiter<EventType>;
        auto awaiter = Awaiter::create(
            m_bus,
            std::function<bool(const EventType&)>(std::forward<Predicate>(predicate)),
            std::function<void(const EventType&)>(std::forward<Callback>(callback)),
            std::move(options),
            single_shot,
            guard());

        std::shared_ptr<IAwaiter> handle = awaiter;
        {
            std::lock_guard<std::mutex> lock(m_awaiters_mutex);
            m_awaiters.emplace_back(handle);
        }
        return handle;
    }

    void prune_awaiters() {
        std::lock_guard<std::mutex> lock(m_awaiters_mutex);
        m_awaiters.erase(std::remove_if(m_awaiters.begin(),
                                        m_awaiters.end(),
                                        [](const std::shared_ptr<IAwaiter>& awaiter) {
                                            return !awaiter || !awaiter->is_active();
                                        }),
                         m_awaiters.end());
    }

private:
    std::weak_ptr<void> guard() const {
        return m_guard;
    }

    EventBus& m_bus;
    std::shared_ptr<void> m_guard;
    bool m_closed = false;
    std::mutex m_awaiters_mutex;
    std::vector<std::shared_ptr<IAwaiter>> m_awaiters;
};

} // namespace event_hub

#endif // EVENT_HUB_EVENT_ENDPOINT_HPP_INCLUDED
