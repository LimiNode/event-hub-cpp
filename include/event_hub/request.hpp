#pragma once
#ifndef EVENT_HUB_REQUEST_HPP_INCLUDED
#define EVENT_HUB_REQUEST_HPP_INCLUDED

/// \file request.hpp
/// \brief Defines request-response helpers and copyable reply callbacks.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace event_hub {

/// \brief Correlation identifier used by request-response event pairs.
using RequestId = std::uint64_t;

/// \brief Invalid request identifier value.
constexpr RequestId invalid_request_id = 0;

/// \class RequestIdGenerator
/// \brief Thread-safe monotonic request id generator.
///
/// Generated ids start from 1. The value 0 is reserved as invalid.
class RequestIdGenerator {
public:
    /// \brief Return the next valid request id.
    /// \return Monotonic request id, never 0 during normal operation.
    RequestId next() noexcept {
        auto id = m_next.fetch_add(1, std::memory_order_relaxed);
        if (id == invalid_request_id) {
            id = m_next.fetch_add(1, std::memory_order_relaxed);
        }
        return id;
    }

private:
    std::atomic<RequestId> m_next{1};
};

/// \class RequestTimeoutError
/// \brief Exception stored in request_future() when a request times out.
class RequestTimeoutError : public std::runtime_error {
public:
    /// \brief Construct an error for a timed-out request.
    /// \param request_id Request id that timed out.
    explicit RequestTimeoutError(RequestId request_id)
        : std::runtime_error("event_hub request timed out: " +
                             std::to_string(request_id)),
          m_request_id(request_id) {}

    /// \brief Return the timed-out request id.
    /// \return Request id associated with this error.
    RequestId request_id() const noexcept {
        return m_request_id;
    }

private:
    RequestId m_request_id = invalid_request_id;
};

/// \class RequestCancelledError
/// \brief Exception stored in request_future() when its cancellation token fires.
class RequestCancelledError : public std::runtime_error {
public:
    /// \brief Construct an error for a cancelled request.
    /// \param request_id Request id that was cancelled.
    explicit RequestCancelledError(RequestId request_id)
        : std::runtime_error("event_hub request cancelled: " +
                             std::to_string(request_id)),
          m_request_id(request_id) {}

    /// \brief Return the cancelled request id.
    /// \return Request id associated with this error.
    RequestId request_id() const noexcept {
        return m_request_id;
    }

private:
    RequestId m_request_id = invalid_request_id;
};

/// \struct RequestTraits
/// \brief Default accessors for events with a public request_id field.
///
/// Specialize this traits type when request and result events use another
/// correlation field name, for example correlation_id.
template <typename EventType>
struct RequestTraits {
    /// \brief Read request id from an event.
    /// \param event Event object.
    /// \return Request id stored in event.request_id.
    static RequestId get_id(const EventType& event) noexcept {
        return event.request_id;
    }

    /// \brief Write request id to an event.
    /// \param event Event object.
    /// \param id Request id to store in event.request_id.
    static void set_id(EventType& event, RequestId id) noexcept {
        event.request_id = id;
    }
};

/// \class Reply
/// \brief Copyable callback wrapper for callback-in-event request shortcuts.
/// \tparam Result Result type passed to the reply callback.
///
/// Reply stores the callback inside shared ownership so events containing a
/// Reply remain copyable. It is a convenience shortcut for tightly coupled
/// flows. Prefer request/result event pairs with RequestId when responses need
/// to be observable, logged, awaited, or handled by several modules.
template <typename Result>
class Reply {
public:
    /// \brief Callback type used by Reply.
    using Callback = std::function<void(const Result&)>;

    /// \brief Construct an empty reply.
    Reply() = default;

    /// \brief Copy a reply callback wrapper.
    Reply(const Reply&) = default;

    /// \brief Move a reply callback wrapper.
    Reply(Reply&&) noexcept = default;

    /// \brief Copy-assign a reply callback wrapper.
    Reply& operator=(const Reply&) = default;

    /// \brief Move-assign a reply callback wrapper.
    Reply& operator=(Reply&&) noexcept = default;

    /// \brief Construct a reply from a callback.
    /// \param callback Callback invoked by operator().
    explicit Reply(Callback callback)
        : m_callback(std::make_shared<Callback>(std::move(callback))) {}

    /// \brief Construct a reply from any compatible callable.
    /// \tparam CallbackLike Callable type invocable as void(const Result&).
    /// \param callback Callback invoked by operator().
    template <typename CallbackLike,
              typename std::enable_if<
                  !std::is_same<typename std::decay<CallbackLike>::type,
                                Reply>::value &&
                      std::is_invocable_r_v<void, CallbackLike&, const Result&>,
                  int>::type = 0>
    explicit Reply(CallbackLike&& callback)
        : Reply(Callback(std::forward<CallbackLike>(callback))) {}

    /// \brief Invoke the stored callback when one exists.
    /// \param result Result object passed to the callback.
    void operator()(const Result& result) const {
        if (m_callback && *m_callback) {
            (*m_callback)(result);
        }
    }

    /// \brief Return true when this reply contains a callback.
    explicit operator bool() const noexcept {
        return m_callback && static_cast<bool>(*m_callback);
    }

    /// \brief Reset this reply to an empty state.
    void reset() noexcept {
        m_callback.reset();
    }

private:
    std::shared_ptr<Callback> m_callback;
};

} // namespace event_hub

#endif // EVENT_HUB_REQUEST_HPP_INCLUDED
