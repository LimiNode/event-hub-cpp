#pragma once
#ifndef EVENT_HUB_RUN_LOOP_HPP_INCLUDED
#define EVENT_HUB_RUN_LOOP_HPP_INCLUDED

/// \file run_loop.hpp
/// \brief Defines a blocking helper loop for passive work sources.

#include "event_bus.hpp"
#include "notifier.hpp"
#include "task_manager.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace event_hub {

/// \class RunLoop
/// \brief Blocking utility loop for EventBus and TaskManager sources.
///
/// RunLoop owns one SyncNotifier and registers it with any number of EventBus
/// and TaskManager instances. Calling run() blocks the current thread, drains
/// all registered sources with process(), and sleeps until the notifier changes
/// or the nearest delayed task deadline is reached.
///
/// This class does not own a worker thread. Registered sources are non-owning
/// pointers and must outlive the RunLoop so their notifier can be reset safely.
/// Add sources before calling run(); source registration is not synchronized
/// with processing.
class RunLoop {
public:
    using Clock = std::chrono::steady_clock; ///< Monotonic clock used for delayed task waits.
    using Duration = Clock::duration; ///< Duration type used for waits.

    /// \brief Construct an empty run loop.
    explicit RunLoop(std::size_t max_tasks_per_manager = 128) noexcept
        : m_max_tasks_per_manager(max_tasks_per_manager == 0
                                      ? 1
                                      : max_tasks_per_manager) {}

    /// \brief Reset registered source notifiers.
    ~RunLoop() {
        reset_sources();
    }

    RunLoop(const RunLoop&) = delete;
    RunLoop& operator=(const RunLoop&) = delete;
    RunLoop(RunLoop&&) = delete;
    RunLoop& operator=(RunLoop&&) = delete;

    /// \brief Return the notifier owned by this loop.
    SyncNotifier& notifier() noexcept {
        return m_notifier;
    }

    /// \brief Return the notifier owned by this loop.
    const SyncNotifier& notifier() const noexcept {
        return m_notifier;
    }

    /// \brief Configure the per-manager task processing limit.
    void set_max_tasks_per_manager(std::size_t max_tasks) noexcept {
        m_max_tasks_per_manager = max_tasks == 0 ? 1 : max_tasks;
    }

    /// \brief Return the per-manager task processing limit.
    std::size_t max_tasks_per_manager() const noexcept {
        return m_max_tasks_per_manager;
    }

    /// \brief Register an EventBus with this loop.
    ///
    /// The bus is stored non-owningly and receives this loop's notifier.
    void add(EventBus& bus) {
        add_bus(bus);
    }

    /// \brief Register an EventBus with this loop.
    void add_bus(EventBus& bus) {
        if (contains(m_buses, &bus)) {
            return;
        }

        bus.set_notifier(&m_notifier);
        m_buses.push_back(&bus);
    }

    /// \brief Register a TaskManager with this loop.
    ///
    /// The manager is stored non-owningly and receives this loop's notifier.
    void add(TaskManager& tasks) {
        add_task_manager(tasks);
    }

    /// \brief Register a TaskManager with this loop.
    void add_task_manager(TaskManager& tasks) {
        if (contains(m_task_managers, &tasks)) {
            return;
        }

        tasks.set_notifier(&m_notifier);
        m_task_managers.push_back(&tasks);
    }

    /// \brief Ask the loop to stop and wake any current wait.
    void request_stop() noexcept {
        m_stop_requested.store(true, std::memory_order_release);
        m_notifier.notify();
    }

    /// \brief Clear a previous stop request.
    void reset_stop() noexcept {
        m_stop_requested.store(false, std::memory_order_release);
    }

    /// \brief Return true when stop has been requested.
    bool stop_requested() const noexcept {
        return m_stop_requested.load(std::memory_order_acquire);
    }

    /// \brief Process each registered source once without waiting.
    ///
    /// \return Total number of events and task callbacks processed.
    std::size_t process_once() {
        std::size_t work_done = 0;
        for (auto* bus : m_buses) {
            work_done += bus->process();
        }

        for (auto* tasks : m_task_managers) {
            work_done += tasks->process(m_max_tasks_per_manager);
        }

        return work_done;
    }

    /// \brief Run until request_stop() is called.
    ///
    /// Exceptions from registered sources propagate out of this function.
    void run() {
        run_until([] {
            return false;
        });
    }

    /// \brief Run until request_stop() or a user predicate stops the loop.
    ///
    /// \tparam StopPredicate Callable returning true when the loop should stop.
    /// \param should_stop Predicate checked at the beginning of each iteration.
    template <typename StopPredicate>
    void run_until(StopPredicate&& should_stop) {
        while (!stop_requested() && !should_stop()) {
            const auto generation = m_notifier.generation();

            const auto work_done = process_once();
            if (work_done != 0) {
                continue;
            }

            if (stop_requested() || should_stop()) {
                break;
            }

            if (has_ready_or_pending_work()) {
                continue;
            }

            const auto wait = next_task_wait();
            if (wait && *wait <= Duration::zero()) {
                continue;
            }

            if (wait) {
                (void)m_notifier.wait_for(generation, *wait);
            } else {
                m_notifier.wait(generation);
            }
        }
    }

    /// \brief Reset notifiers on every registered source and forget sources.
    void reset_sources() noexcept {
        for (auto* bus : m_buses) {
            bus->reset_notifier();
        }
        m_buses.clear();

        for (auto* tasks : m_task_managers) {
            tasks->reset_notifier();
        }
        m_task_managers.clear();
    }

private:
    template <typename T>
    static bool contains(const std::vector<T*>& values, T* value) {
        return std::find(values.begin(), values.end(), value) != values.end();
    }

    bool has_ready_or_pending_work() const {
        for (const auto* bus : m_buses) {
            if (bus->has_pending()) {
                return true;
            }
        }

        for (const auto* tasks : m_task_managers) {
            if (tasks->has_ready()) {
                return true;
            }
        }

        return false;
    }

    std::optional<Duration> next_task_wait() {
        std::optional<Duration> best;
        const auto now = Clock::now();

        for (auto* bus : m_buses) {
            const auto deadline = bus->next_awaiter_deadline();
            if (!deadline) {
                continue;
            }

            const auto wait =
                *deadline <= now ? Duration::zero() : (*deadline - now);
            if (!best || wait < *best) {
                best = wait;
            }
        }

        for (auto* tasks : m_task_managers) {
            if (tasks->has_ready()) {
                return Duration::zero();
            }

            const auto deadline = tasks->next_deadline();
            if (!deadline) {
                continue;
            }

            const auto wait =
                *deadline <= now ? Duration::zero() : (*deadline - now);
            if (!best || wait < *best) {
                best = wait;
            }
        }

        return best;
    }

private:
    SyncNotifier m_notifier;
    std::vector<EventBus*> m_buses;
    std::vector<TaskManager*> m_task_managers;
    std::size_t m_max_tasks_per_manager = 128;
    std::atomic<bool> m_stop_requested{false};
};

} // namespace event_hub

#endif // EVENT_HUB_RUN_LOOP_HPP_INCLUDED
