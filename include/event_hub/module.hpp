#pragma once
#ifndef EVENT_HUB_MODULE_HPP_INCLUDED
#define EVENT_HUB_MODULE_HPP_INCLUDED

/// \file module.hpp
/// \brief Defines the base module abstraction for modular applications.

#include "event_node.hpp"
#include "notifier.hpp"
#include "task_manager.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace event_hub {

class ModuleHub;

/// \brief Defines how a module task manager is processed.
enum class ModuleExecutionMode : std::uint8_t {
    inline_in_hub, ///< Module tasks are processed by ModuleHub::process().
    private_thread, ///< Module tasks are processed by the module's private worker thread.
    manual ///< Module tasks are owned by the module but processed by external code.
};

/// \brief Configuration for Module task execution.
struct ModuleOptions {
    ModuleExecutionMode execution{ModuleExecutionMode::inline_in_hub}; ///< Task manager execution mode.
    std::size_t max_tasks_per_process{64}; ///< Maximum task callbacks processed for this module in one pass.
};

/// \class Module
/// \brief EventNode with an owned TaskManager and explicit lifecycle hooks.
///
/// A Module connects to one EventBus through EventNode and owns one passive
/// TaskManager for its own work. ModuleHub decides whether that task manager is
/// processed inline, by a private worker thread, or manually by external code.
///
/// Lifecycle methods are single-consumer operations. Producers may still post
/// events to the shared bus or tasks to the module task manager according to
/// the EventBus and TaskManager threading rules.
class Module : public EventNode {
public:
    using TimePoint = TaskManager::TimePoint; ///< Monotonic time point used for module deadline hints.

    /// \brief Construct a module connected to a shared bus.
    explicit Module(EventBus& bus, ModuleOptions options = {})
        : EventNode(bus),
          m_options(normalize_options(options)) {}

    /// \brief Destroy the module.
    ~Module() override = default;

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    Module(Module&&) = delete;
    Module& operator=(Module&&) = delete;

    /// \brief Return this module's task manager.
    TaskManager& tasks() noexcept {
        return m_tasks;
    }

    /// \brief Return this module's task manager.
    const TaskManager& tasks() const noexcept {
        return m_tasks;
    }

    /// \brief Return the configured execution mode.
    ModuleExecutionMode execution_mode() const noexcept {
        return m_options.execution;
    }

    /// \brief Return the per-pass task processing quota.
    std::size_t max_tasks_per_process() const noexcept {
        return m_options.max_tasks_per_process;
    }

    /// \brief Return true after initialize() succeeds and before shutdown().
    bool is_initialized() const noexcept {
        return m_initialized.load(std::memory_order_acquire);
    }

    /// \brief Return true while shutdown is in progress.
    bool is_stopping() const noexcept {
        return m_stopping.load(std::memory_order_acquire);
    }

    /// \brief Return true after shutdown() has completed.
    bool is_stopped() const noexcept {
        return m_stopped.load(std::memory_order_acquire);
    }

    /// \brief Initialize this module once.
    ///
    /// The module cannot be initialized again after shutdown(), because its
    /// TaskManager has been closed.
    void initialize() {
        if (is_initialized()) {
            return;
        }
        if (is_stopping() || is_stopped()) {
            throw std::logic_error("Module cannot be initialized after shutdown");
        }

        on_initialize();
        m_initialized.store(true, std::memory_order_release);
    }

    /// \brief Run one lightweight lifecycle processing hook.
    ///
    /// This does not process the module task manager. Task processing is
    /// controlled by ModuleHub or external code according to execution_mode().
    std::size_t process() {
        rethrow_worker_exception();
        if (!is_initialized() || is_stopping() || is_stopped()) {
            return 0;
        }

        return on_process();
    }

    /// \brief Stop this module and release owned subscriptions and tasks.
    ///
    /// This operation is idempotent and does not throw. It does not wait for
    /// EventBus callbacks that already started before endpoint closure.
    void shutdown() noexcept {
        if (m_stopped.load(std::memory_order_acquire)) {
            // A private worker can request shutdown from inside its own
            // callback. The self-join is intentionally deferred; a later
            // shutdown call (or the owning hub) must still join that thread.
            join_worker_noexcept();
            return;
        }
        if (m_stopping.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        request_worker_stop();
        if (m_worker.joinable() &&
            m_worker.get_id() == std::this_thread::get_id()) {
            // A worker cannot join itself. Defer the entire teardown until
            // the owning thread joins it; otherwise the remainder of the
            // current TaskManager batch could run against torn-down state.
            m_stopping.store(false, std::memory_order_release);
            return;
        }
        join_worker_noexcept();

        if (m_initialized.load(std::memory_order_acquire)) {
            try {
                on_shutdown();
            } catch (...) {
            }
        }

        m_tasks.close();
        m_tasks.clear_pending();
        close();

        m_initialized.store(false, std::memory_order_release);
        m_stopped.store(true, std::memory_order_release);
        m_stopping.store(false, std::memory_order_release);
    }

protected:
    /// \brief Called once by initialize().
    virtual void on_initialize() {}

    /// \brief Called by ModuleHub::process() for main-loop work.
    ///
    /// Return the amount of work completed so the hub can avoid sleeping.
    virtual std::size_t on_process() {
        return 0;
    }

    /// \brief Optional main-loop wake-up hint for external integrations.
    virtual std::optional<TimePoint> next_deadline_hint() const {
        return std::nullopt;
    }

    /// \brief Called once by shutdown() after private workers stop.
    virtual void on_shutdown() noexcept {}

private:
    friend class ModuleHub;

    static ModuleOptions normalize_options(ModuleOptions options) noexcept {
        if (options.max_tasks_per_process == 0U) {
            options.max_tasks_per_process = 1U;
        }
        return options;
    }

    void set_worker_exception_notifier(INotifier* notifier) noexcept {
        m_worker_exception_notifier.store(notifier, std::memory_order_release);
    }

    void start_worker() {
        if (m_options.execution != ModuleExecutionMode::private_thread) {
            return;
        }
        if (m_worker.joinable()) {
            return;
        }

        clear_worker_exception();
        m_worker_stop_requested.store(false, std::memory_order_release);
        m_tasks.set_notifier(&m_worker_notifier);

        m_worker = std::thread([this] {
            run_worker_loop();
        });
    }

    void request_worker_stop() noexcept {
        m_worker_stop_requested.store(true, std::memory_order_release);
        m_worker_notifier.notify();
    }

    void join_worker() {
        if (m_worker.joinable()) {
            if (m_worker.get_id() == std::this_thread::get_id()) {
                throw std::logic_error("Module cannot join its own worker");
            }
            m_worker.join();
        }

        m_tasks.reset_notifier();
        rethrow_worker_exception();
    }

    void join_worker_noexcept() noexcept {
        try {
            if (m_worker.joinable() &&
                m_worker.get_id() != std::this_thread::get_id()) {
                m_worker.join();
            }
            m_tasks.reset_notifier();
        } catch (...) {
        }
    }

    bool worker_stop_requested() const noexcept {
        return m_worker_stop_requested.load(std::memory_order_acquire);
    }

    void run_worker_loop() noexcept {
        try {
            while (!worker_stop_requested()) {
                const auto generation = m_worker_notifier.generation();

                const auto work_done =
                    m_tasks.process(m_options.max_tasks_per_process);
                if (work_done != 0U) {
                    continue;
                }

                if (worker_stop_requested()) {
                    break;
                }

                if (m_tasks.has_ready()) {
                    continue;
                }

                const auto deadline = m_tasks.next_deadline();
                if (!deadline) {
                    m_worker_notifier.wait(generation);
                    continue;
                }

                const auto now = TaskManager::Clock::now();
                if (*deadline <= now) {
                    continue;
                }

                (void)m_worker_notifier.wait_for(generation, *deadline - now);
            }
        } catch (...) {
            store_worker_exception(std::current_exception());
            m_worker_stop_requested.store(true, std::memory_order_release);
            notify_worker_exception();
        }
    }

    std::optional<TimePoint> deadline_hint() const {
        return next_deadline_hint();
    }

    void clear_worker_exception() noexcept {
        std::lock_guard<std::mutex> lock(m_worker_exception_mutex);
        m_worker_exception = nullptr;
    }

    void store_worker_exception(std::exception_ptr exception) noexcept {
        try {
            std::lock_guard<std::mutex> lock(m_worker_exception_mutex);
            if (!m_worker_exception) {
                m_worker_exception = std::move(exception);
            }
        } catch (...) {
        }
    }

    void rethrow_worker_exception() {
        std::exception_ptr exception;
        {
            std::lock_guard<std::mutex> lock(m_worker_exception_mutex);
            exception = m_worker_exception;
            m_worker_exception = nullptr;
        }

        if (exception) {
            std::rethrow_exception(exception);
        }
    }

    void notify_worker_exception() noexcept {
        if (auto* notifier =
                m_worker_exception_notifier.load(std::memory_order_acquire)) {
            notifier->notify();
        }
    }

private:
    TaskManager m_tasks;
    ModuleOptions m_options;

    std::atomic_bool m_initialized{false};
    std::atomic_bool m_stopping{false};
    std::atomic_bool m_stopped{false};

    SyncNotifier m_worker_notifier;
    std::thread m_worker;
    std::atomic_bool m_worker_stop_requested{false};
    std::atomic<INotifier*> m_worker_exception_notifier{nullptr};
    mutable std::mutex m_worker_exception_mutex;
    std::exception_ptr m_worker_exception;
};

} // namespace event_hub

#endif // EVENT_HUB_MODULE_HPP_INCLUDED
