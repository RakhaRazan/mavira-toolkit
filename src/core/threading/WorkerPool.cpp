#include "core/threading/WorkerPool.hpp"

#include <algorithm>
#include <utility>

namespace mavira::core::threading {

WorkerPool::WorkerPool(unsigned int requestedWorkers) {
    const unsigned int hw = (std::max)(1u, std::thread::hardware_concurrency());
    const unsigned int cap = (std::min)(kMaxWorkers, hw);
    const unsigned int count = (requestedWorkers == 0)
        ? cap
        : (std::clamp)(requestedWorkers, 1u, cap);

    workers_.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
        workers_.emplace_back([this](std::stop_token st) {
            workerLoop(std::move(st));
        });
    }
}

WorkerPool::~WorkerPool() {
    shutdown();
}

TaskHandle WorkerPool::enqueue(
    mavira::core::RequestPriority priority,
    std::function<void(std::stop_token)> task,
    std::stop_token externalStopToken) {
    auto stopSource = std::make_shared<std::stop_source>();
    const std::uint64_t seq = nextSequence_.fetch_add(1, std::memory_order_relaxed);

    {
        std::scoped_lock lock(mutex_);
        if (shuttingDown_) {
            stopSource->request_stop();
            return TaskHandle{seq, std::move(stopSource)};
        }

        queue_.push(QueuedTask{
            priority,
            seq,
            stopSource,
            std::move(externalStopToken),
            std::move(task)
        });
    }
    cv_.notify_one();

    return TaskHandle{seq, std::move(stopSource)};
}

void WorkerPool::cancelAllPending() {
    std::scoped_lock lock(mutex_);
    while (!queue_.empty()) {
        if (queue_.top().stopSource != nullptr) {
            queue_.top().stopSource->request_stop();
        }
        queue_.pop();
    }
}

void WorkerPool::shutdown() {
    {
        std::scoped_lock lock(mutex_);
        if (shuttingDown_) {
            return;
        }
        shuttingDown_ = true;
        while (!queue_.empty()) {
            if (queue_.top().stopSource != nullptr) {
                queue_.top().stopSource->request_stop();
            }
            queue_.pop();
        }
    }

    for (auto& worker : workers_) {
        worker.request_stop();
    }
    cv_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

std::size_t WorkerPool::workerCount() const noexcept {
    return workers_.size();
}

std::size_t WorkerPool::pendingCount() const {
    std::scoped_lock lock(mutex_);
    return queue_.size();
}

void WorkerPool::workerLoop(std::stop_token workerStop) {
    for (;;) {
        QueuedTask task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, workerStop, [this]() {
                return shuttingDown_ || !queue_.empty();
            });

            if (workerStop.stop_requested() || (shuttingDown_ && queue_.empty())) {
                return;
            }
            if (queue_.empty()) {
                continue;
            }

            task = std::move(const_cast<QueuedTask&>(queue_.top()));
            queue_.pop();
        }

        if (!task.fn) {
            continue;
        }

        // Skip immediately if already cancelled prior to execution.
        if ((task.stopSource && task.stopSource->stop_requested()) ||
            task.externalStopToken.stop_requested()) {
            continue;
        }

        // Forward both worker shutdown and external stop token into the task's stop_source.
        std::stop_callback workerCb(workerStop, [&]() noexcept {
            if (task.stopSource) {
                task.stopSource->request_stop();
            }
        });
        std::stop_callback externalCb(task.externalStopToken, [&]() noexcept {
            if (task.stopSource) {
                task.stopSource->request_stop();
            }
        });

        task.fn(task.stopSource ? task.stopSource->get_token() : workerStop);
    }
}

} // namespace mavira::core::threading
