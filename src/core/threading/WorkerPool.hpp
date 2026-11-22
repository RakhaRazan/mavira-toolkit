#pragma once

#include "core/CommonTypes.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace mavira::core::threading {

/// Handle returned when enqueuing work on `WorkerPool`, allowing cooperative cancellation.
struct TaskHandle {
    std::uint64_t id{0};
    std::shared_ptr<std::stop_source> stopSource{};

    void cancel() const noexcept {
        if (stopSource != nullptr) {
            stopSource->request_stop();
        }
    }

    [[nodiscard]] bool isCancellationRequested() const noexcept {
        return stopSource != nullptr && stopSource->stop_requested();
    }
};

/// Priority task scheduler backed by `std::jthread` workers (capped at
/// `std::min(4u, std::thread::hardware_concurrency())`) with `std::stop_token`
/// cooperative cancellation.
class WorkerPool {
public:
    static constexpr unsigned int kMaxWorkers = 4u;

    /// Constructs the pool. When `requestedWorkers == 0`, uses
    /// `std::min(4u, std::max(1u, std::thread::hardware_concurrency()))`.
    explicit WorkerPool(unsigned int requestedWorkers = 0);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&&) = delete;
    WorkerPool& operator=(WorkerPool&&) = delete;

    /// Enqueues a task at `priority` that receives a cooperative `std::stop_token`.
    /// If `externalStopToken` is stop-requested, the task's token is also cancelled.
    TaskHandle enqueue(
        mavira::core::RequestPriority priority,
        std::function<void(std::stop_token)> task,
        std::stop_token externalStopToken = {});

    /// Submits a callable `R(std::stop_token)` and returns `{TaskHandle, std::future<R>}`.
    template <typename Fn>
    auto submit(
        mavira::core::RequestPriority priority,
        Fn&& fn,
        std::stop_token externalStopToken = {}) {
        using ResultT = std::invoke_result_t<Fn, std::stop_token>;
        auto promise = std::make_shared<std::promise<ResultT>>();
        std::future<ResultT> future = promise->get_future();

        TaskHandle handle = enqueue(
            priority,
            [fn = std::forward<Fn>(fn), promise = std::move(promise)](std::stop_token st) mutable {
                try {
                    if constexpr (std::is_void_v<ResultT>) {
                        fn(st);
                        promise->set_value();
                    } else {
                        promise->set_value(fn(st));
                    }
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            },
            std::move(externalStopToken));

        return std::make_pair(std::move(handle), std::move(future));
    }

    /// Requests cancellation on all currently queued (not yet started) tasks.
    void cancelAllPending();

    /// Stops all worker threads and joins them cleanly.
    void shutdown();

    [[nodiscard]] std::size_t workerCount() const noexcept;
    [[nodiscard]] std::size_t pendingCount() const;

private:
    struct QueuedTask {
        mavira::core::RequestPriority priority{mavira::core::RequestPriority::Normal};
        std::uint64_t sequence{0};
        std::shared_ptr<std::stop_source> stopSource{};
        std::stop_token externalStopToken{};
        std::function<void(std::stop_token)> fn{};
    };

    struct TaskComparator {
        [[nodiscard]] bool operator()(const QueuedTask& a, const QueuedTask& b) const noexcept {
            const auto pa = static_cast<std::uint8_t>(a.priority);
            const auto pb = static_cast<std::uint8_t>(b.priority);
            if (pa != pb) {
                return pa < pb; // Higher RequestPriority pops first
            }
            return a.sequence > b.sequence; // FIFO within equal priority
        }
    };

    void workerLoop(std::stop_token workerStop);

    mutable std::mutex mutex_{};
    std::condition_variable_any cv_{};
    std::priority_queue<QueuedTask, std::vector<QueuedTask>, TaskComparator> queue_{};
    std::vector<std::jthread> workers_{};
    std::atomic<std::uint64_t> nextSequence_{1};
    bool shuttingDown_{false};
};

} // namespace mavira::core::threading
