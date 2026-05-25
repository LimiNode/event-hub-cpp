#pragma once
#ifndef EVENT_HUB_MODULE_HUB_HPP_INCLUDED
#define EVENT_HUB_MODULE_HUB_HPP_INCLUDED

/// \file module_hub.hpp
/// \brief Defines an orchestration hub for event_hub modules.

#include "event_bus.hpp"
#include "module.hpp"
#include "notifier.hpp"
#include "task_manager.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace event_hub {

/// \class ModuleHub
/// \brief Owns a shared EventBus and a collection of modules.
///
/// ModuleHub is a passive work source first: process() performs one
/// non-blocking pass over the shared bus, inline module task managers, and
/// module lifecycle hooks. run() and start() are convenience wrappers that use
/// the same passive processing model with notifier/deadline based waiting.
///
/// Module registration is not synchronized with processing. Add modules before
/// initialize(), run(), or start().
class ModuleHub {
public:
    using TimePoint = TaskManager::TimePoint; ///< Monotonic time point used for deadline aggregation.
    using ModulePtr = std::unique_ptr<Module>; ///< Owned module pointer type.

    /// \brief Construct an empty hub.
    ModuleHub() = default;

    /// \brief Stop background work and release notifiers.
    ~ModuleHub() noexcept {
        try {
            request_stop();
            if (m_thread.joinable() &&
                m_thread.get_id() != std::this_thread::get_id()) {
                m_thread.join();
            }
            shutdown();
            reset_notifier();
        } catch (...) {
        }
    }

    ModuleHub(const ModuleHub&) = delete;
    ModuleHub& operator=(const ModuleHub&) = delete;
    ModuleHub(ModuleHub&&) = delete;
    ModuleHub& operator=(ModuleHub&&) = delete;

    /// \brief Return the shared event bus.
    EventBus& bus() noexcept {
        return m_bus;
    }

    /// \brief Return the shared event bus.
    const EventBus& bus() const noexcept {
        return m_bus;
    }

    /// \brief Construct and add a module connected to this hub's bus.
    ///
    /// ModuleType must derive from event_hub::Module and be constructible with
    /// `(EventBus&, Args&&...)`.
    template <typename ModuleType, typename... Args>
    ModuleType& emplace_module(Args&&... args) {
        static_assert(std::is_base_of<Module, ModuleType>::value,
                      "ModuleType must derive from event_hub::Module");

        auto module = std::make_unique<ModuleType>(
            m_bus,
            std::forward<Args>(args)...);
        auto& ref = *module;
        add_module(std::move(module));
        return ref;
    }

    /// \brief Add an already constructed module.
    void add_module(ModulePtr module) {
        if (!module) {
            throw std::invalid_argument("ModuleHub cannot add a null module");
        }
        if (m_initialized.load(std::memory_order_acquire) ||
            m_running.load(std::memory_order_acquire)) {
            throw std::logic_error(
                "ModuleHub modules must be added before initialization");
        }
        if (&module->bus() != &m_bus) {
            throw std::invalid_argument(
                "ModuleHub module is connected to a different EventBus");
        }

        wire_module_notifier(*module, current_notifier());
        m_modules.emplace_back(std::move(module));
    }

    /// \brief Return the number of owned modules.
    std::size_t module_count() const noexcept {
        return m_modules.size();
    }

    /// \brief Initialize all modules in registration order.
    ///
    /// Private module workers are started only after every module has
    /// initialized successfully.
    void initialize() {
        if (m_initialized.load(std::memory_order_acquire)) {
            return;
        }

        try {
            for (auto& module : m_modules) {
                module->initialize();
            }

            for (auto& module : m_modules) {
                module->start_worker();
            }

            m_initialized.store(true, std::memory_order_release);
        } catch (...) {
            for (auto& module : m_modules) {
                module->request_worker_stop();
            }
            for (auto& module : m_modules) {
                module->join_worker_noexcept();
            }

            for (auto it = m_modules.rbegin(); it != m_modules.rend(); ++it) {
                (*it)->shutdown();
            }
            throw;
        }
    }

    /// \brief Request all active loops and private workers to stop.
    void request_stop() noexcept {
        m_stop_requested.store(true, std::memory_order_release);

        if (auto* notifier = current_notifier()) {
            notifier->notify();
        }
        m_local_notifier.notify();

        for (auto& module : m_modules) {
            module->request_worker_stop();
        }
    }

    /// \brief Return true when stop has been requested.
    bool stop_requested() const noexcept {
        return m_stop_requested.load(std::memory_order_acquire);
    }

    /// \brief Process one passive hub pass without waiting.
    ///
    /// The pass drains the shared bus snapshot, processes inline module task
    /// managers with per-module quotas, then calls each initialized module's
    /// process hook. Events posted by tasks or hooks are dispatched by a later
    /// process() call, matching EventBus snapshot semantics.
    std::size_t process() {
        std::size_t work_done = 0;

        work_done += m_bus.process();

        for (auto& module : m_modules) {
            module->rethrow_worker_exception();
            if (module->execution_mode() !=
                    ModuleExecutionMode::inline_in_hub ||
                !module->is_initialized() || module->is_stopping() ||
                module->is_stopped()) {
                continue;
            }

            work_done += module->tasks().process(
                module->max_tasks_per_process());
        }

        for (auto& module : m_modules) {
            work_done += module->process();
        }

        return work_done;
    }

    /// \brief Alias for process(), matching RunLoop naming.
    std::size_t process_once() {
        return process();
    }

    /// \brief Return true when immediate hub work can be processed.
    bool has_pending() const {
        if (m_bus.has_pending()) {
            return true;
        }

        const auto now = TaskManager::Clock::now();
        for (const auto& module : m_modules) {
            if (!module->is_initialized() || module->is_stopping() ||
                module->is_stopped()) {
                continue;
            }

            if (module->execution_mode() ==
                ModuleExecutionMode::inline_in_hub) {
                const auto& tasks = module->tasks();
                if (tasks.has_ready()) {
                    return true;
                }

                auto& mutable_tasks =
                    const_cast<TaskManager&>(module->tasks());
                const auto deadline = mutable_tasks.next_deadline();
                if (deadline && *deadline <= now) {
                    return true;
                }
            }

            const auto hint = module->deadline_hint();
            if (hint && *hint <= now) {
                return true;
            }
        }

        return false;
    }

    /// \brief Return the earliest future hub deadline, if any.
    std::optional<TimePoint> next_deadline() {
        std::optional<TimePoint> best;

        for (auto& module : m_modules) {
            if (!module->is_initialized() || module->is_stopping() ||
                module->is_stopped()) {
                continue;
            }

            if (module->execution_mode() ==
                ModuleExecutionMode::inline_in_hub) {
                update_best_deadline(best, module->tasks().next_deadline());
            }

            update_best_deadline(best, module->deadline_hint());
        }

        return best;
    }

    /// \brief Set a non-owning notifier for passive embedding.
    ///
    /// The notifier is installed on the shared bus and inline module task
    /// managers. Private-thread modules use their own worker notifiers.
    void set_notifier(INotifier* notifier) noexcept {
        m_notifier.store(notifier, std::memory_order_release);
        wire_sources_notifier(notifier);
    }

    /// \brief Remove the currently configured passive notifier.
    void reset_notifier() noexcept {
        m_notifier.store(nullptr, std::memory_order_release);
        wire_sources_notifier(nullptr);
    }

    /// \brief Run a blocking active loop on the calling thread.
    ///
    /// This method initializes modules before entering the loop and shuts them
    /// down before returning. Exceptions from processing are rethrown after
    /// shutdown.
    void run() {
        bool expected = false;
        if (!m_running.compare_exchange_strong(expected,
                                               true,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
            throw std::logic_error("ModuleHub is already running");
        }

        try {
            clear_background_exception();
            m_stop_requested.store(false, std::memory_order_release);
            set_notifier(&m_local_notifier);
            initialize();
            run_loop();
            shutdown();
            reset_notifier();
            m_running.store(false, std::memory_order_release);
        } catch (...) {
            try {
                shutdown();
                reset_notifier();
            } catch (...) {
            }
            m_running.store(false, std::memory_order_release);
            throw;
        }
    }

    /// \brief Start the active loop in a background thread.
    void start() {
        if (m_thread.joinable()) {
            throw std::logic_error("ModuleHub background thread is joinable");
        }

        bool expected = false;
        if (!m_running.compare_exchange_strong(expected,
                                               true,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
            throw std::logic_error("ModuleHub is already running");
        }

        clear_background_exception();
        m_stop_requested.store(false, std::memory_order_release);
        try {
            m_thread = std::thread([this] {
                try {
                    set_notifier(&m_local_notifier);
                    initialize();
                    run_loop();
                    shutdown();
                    reset_notifier();
                } catch (...) {
                    store_background_exception(std::current_exception());
                    try {
                        shutdown();
                        reset_notifier();
                    } catch (...) {
                    }
                }

                m_running.store(false, std::memory_order_release);
            });
        } catch (...) {
            m_running.store(false, std::memory_order_release);
            throw;
        }
    }

    /// \brief Join the background thread and rethrow a stored exception.
    void join() {
        if (m_thread.joinable()) {
            if (m_thread.get_id() == std::this_thread::get_id()) {
                throw std::logic_error("ModuleHub cannot join its own thread");
            }
            m_thread.join();
        }

        rethrow_background_exception();
        rethrow_module_worker_exception();
    }

    /// \brief Stop workers and shut down modules in reverse order.
    void shutdown() noexcept {
        request_stop();

        for (auto& module : m_modules) {
            module->request_worker_stop();
        }
        for (auto& module : m_modules) {
            module->join_worker_noexcept();
        }

        for (auto it = m_modules.rbegin(); it != m_modules.rend(); ++it) {
            (*it)->shutdown();
        }

        m_bus.clear_pending();
        m_initialized.store(false, std::memory_order_release);
    }

private:
    static void update_best_deadline(std::optional<TimePoint>& best,
                                     std::optional<TimePoint> candidate) {
        if (!candidate) {
            return;
        }
        if (!best || *candidate < *best) {
            best = *candidate;
        }
    }

    INotifier* current_notifier() const noexcept {
        return m_notifier.load(std::memory_order_acquire);
    }

    void wire_sources_notifier(INotifier* notifier) noexcept {
        if (notifier) {
            m_bus.set_notifier(notifier);
        } else {
            m_bus.reset_notifier();
        }

        for (auto& module : m_modules) {
            wire_module_notifier(*module, notifier);
        }
    }

    void wire_module_notifier(Module& module, INotifier* notifier) noexcept {
        module.set_worker_exception_notifier(notifier);

        if (module.execution_mode() != ModuleExecutionMode::inline_in_hub) {
            return;
        }

        if (notifier) {
            module.tasks().set_notifier(notifier);
        } else {
            module.tasks().reset_notifier();
        }
    }

    void run_loop() {
        while (!stop_requested()) {
            const auto generation = m_local_notifier.generation();

            const auto work_done = process();
            if (work_done != 0U) {
                continue;
            }

            if (stop_requested()) {
                break;
            }

            if (has_pending()) {
                continue;
            }

            const auto deadline = next_deadline();
            if (!deadline) {
                m_local_notifier.wait(generation);
                continue;
            }

            const auto now = TaskManager::Clock::now();
            if (*deadline <= now) {
                continue;
            }

            (void)m_local_notifier.wait_for(generation, *deadline - now);
        }
    }

    void clear_background_exception() noexcept {
        std::lock_guard<std::mutex> lock(m_background_exception_mutex);
        m_background_exception = nullptr;
    }

    void store_background_exception(std::exception_ptr exception) noexcept {
        try {
            std::lock_guard<std::mutex> lock(m_background_exception_mutex);
            if (!m_background_exception) {
                m_background_exception = std::move(exception);
            }
        } catch (...) {
        }
    }

    void rethrow_background_exception() {
        std::exception_ptr exception;
        {
            std::lock_guard<std::mutex> lock(m_background_exception_mutex);
            exception = m_background_exception;
            m_background_exception = nullptr;
        }

        if (exception) {
            std::rethrow_exception(exception);
        }
    }

    void rethrow_module_worker_exception() {
        for (auto& module : m_modules) {
            module->rethrow_worker_exception();
        }
    }

private:
    EventBus m_bus;
    std::vector<ModulePtr> m_modules;

    SyncNotifier m_local_notifier;
    std::atomic<INotifier*> m_notifier{nullptr};

    std::atomic_bool m_initialized{false};
    std::atomic_bool m_stop_requested{false};
    std::atomic_bool m_running{false};

    std::thread m_thread;
    mutable std::mutex m_background_exception_mutex;
    std::exception_ptr m_background_exception;
};

} // namespace event_hub

#endif // EVENT_HUB_MODULE_HUB_HPP_INCLUDED
