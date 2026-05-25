#pragma once
#ifndef EVENT_HUB_NOTIFIER_HPP_INCLUDED
#define EVENT_HUB_NOTIFIER_HPP_INCLUDED

/// \file notifier.hpp
/// \brief Defines external wake-up notification primitives for event loops.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace event_hub {

/// \class INotifier
/// \brief Minimal non-owning wake-up target used by event producers.
///
/// EventBus calls notify() after new queued work is posted. Implementations
/// decide how the application event loop should be woken.
///
/// \warning EventBus stores notifier pointers non-owningly. The notifier must
/// outlive every producer that can call EventBus::post(), or the caller must
/// reset the bus notifier before destroying it.
class INotifier {
public:
    /// \brief Destroy notifier interface.
    virtual ~INotifier() = default;

    /// \brief Signal that pending work may now be available.
    virtual void notify() noexcept = 0;
};

/// \class SyncNotifier
/// \brief Condition-variable notifier with a generation counter.
///
/// The generation counter lets callers record a known state before processing
/// work and then wait only if no notification happened in the meantime.
///
/// This is useful when a single application-owned loop combines EventBus work
/// with other queues. It prevents a notification posted between generation()
/// and wait_for() from being lost.
class SyncNotifier final : public INotifier {
public:
    using Clock = std::chrono::steady_clock; ///< Monotonic clock used for wait durations.

    /// \brief Increment the generation and wake all waiters.
    void notify() noexcept override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_generation;
        }

        m_cv.notify_all();
    }

    /// \brief Return the current notification generation.
    /// \return Current generation value.
    std::uint64_t generation() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_generation;
    }

    /// \brief Wait until the notification generation changes.
    /// \param old_generation Generation value observed before waiting.
    void wait(std::uint64_t old_generation) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this, old_generation] {
            return m_generation != old_generation;
        });
    }

    /// \brief Wait until the generation changes or the timeout expires.
    /// \tparam Rep Duration representation type.
    /// \tparam Period Duration period type.
    /// \param old_generation Generation value observed before waiting.
    /// \param timeout Maximum time to wait for a generation change.
    /// \return True when notified, false when the timeout expires first.
    template <typename Rep, typename Period>
    bool wait_for(std::uint64_t old_generation,
                  const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [this, old_generation] {
            return m_generation != old_generation;
        });
    }

    /// \brief Wait until the generation changes or timeout_ms expires.
    bool wait_for_ms(std::uint64_t old_generation, std::int64_t timeout_ms) {
        return wait_for(old_generation, std::chrono::milliseconds(timeout_ms));
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::uint64_t m_generation{0};
};

} // namespace event_hub

#endif // EVENT_HUB_NOTIFIER_HPP_INCLUDED
