#pragma once
#ifndef EVENT_HUB_AWAITER_INTERFACES_HPP_INCLUDED
#define EVENT_HUB_AWAITER_INTERFACES_HPP_INCLUDED

/// \file awaiter_interfaces.hpp
/// \brief Interfaces for cancelable event awaiters.

#include <cstdint>

namespace event_hub {

/// \brief Source that is currently polling or dispatching event callbacks.
enum class DispatchSource : std::uint8_t {
    direct, ///< Immediate caller-thread dispatch from emit_direct().
    queued ///< Queued dispatch from process() or a run loop.
};

/// \class IAwaiter
/// \brief Minimal cancelable awaiter handle.
///
/// Awaiter handles are returned by EventEndpoint waiting helpers. They can be
/// cancelled explicitly, by endpoint shutdown, or by their own timeout and
/// cancellation-token rules.
class IAwaiter {
public:
    /// \brief Cancel the awaiter.
    ///
    /// The operation is idempotent and releases the awaiter's bus
    /// subscription when one is still active.
    virtual void cancel() noexcept = 0;

    /// \brief Return true while the awaiter can still receive events.
    /// \return True while the awaiter is active.
    virtual bool is_active() const noexcept = 0;

    /// \brief Destroy the awaiter handle.
    virtual ~IAwaiter() = default;
};

/// \class IAwaiterEx
/// \brief Awaiter interface with timeout and cancellation polling.
///
/// EventBus polls this extended interface after emit_direct() and process() so
/// timeout and cancellation-token state can stop awaiters without a background
/// thread.
class IAwaiterEx : public IAwaiter {
public:
    /// \brief Poll timeout and cancellation conditions.
    /// \param source Dispatch source that reached the polling point.
    virtual void poll_timeout(DispatchSource source) noexcept = 0;

    /// \brief Destroy the extended awaiter handle.
    ~IAwaiterEx() override = default;
};

} // namespace event_hub

#endif // EVENT_HUB_AWAITER_INTERFACES_HPP_INCLUDED
