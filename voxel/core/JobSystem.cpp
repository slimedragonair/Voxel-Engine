#include <voxel/core/JobSystem.hpp>

#include <algorithm>
#include <cassert>

namespace voxel::core {

namespace {

thread_local bool g_isWorker = false;

} // namespace

JobSystem::JobSystem() = default;

JobSystem::~JobSystem()
{
    stop();
}

void JobSystem::start(std::size_t workerCount)
{
    if (running_.load(std::memory_order_acquire)) {
        return;
    }

    if (workerCount == 0) {
        const auto hw = static_cast<std::size_t>(std::thread::hardware_concurrency());
        const auto automatic = (hw > 2) ? (hw - 2) : 1;
        workerCount = std::clamp<std::size_t>(automatic, 1, 10);
    }

    stopping_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    workers_.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i) {
        workers_.emplace_back([this]() {
            g_isWorker = true;
            workerLoop();
            g_isWorker = false;
        });
    }
}

void JobSystem::stop()
{
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_.store(true, std::memory_order_release);
    }
    cv_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& q : queues_) {
            q.clear();
        }
        pendingTotal_.store(0, std::memory_order_release);
        activeCount_.store(0, std::memory_order_release);
    }
    running_.store(false, std::memory_order_release);
    stopping_.store(false, std::memory_order_release);
    idleCv_.notify_all();
}

bool JobSystem::running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

std::size_t JobSystem::workerCount() const noexcept
{
    return workers_.size();
}

std::size_t JobSystem::pendingCount() const noexcept
{
    return pendingTotal_.load(std::memory_order_acquire);
}

void JobSystem::waitAll()
{
    assert(!g_isWorker && "JobSystem::waitAll() must not be called from a worker thread");
    std::unique_lock<std::mutex> lock(mutex_);
    idleCv_.wait(lock, [this]() {
        return pendingTotal_.load(std::memory_order_acquire) == 0
            && activeCount_.load(std::memory_order_acquire) == 0;
    });
}

void JobSystem::enqueue(JobPriority priority, Task task)
{
    const auto slot = static_cast<std::size_t>(priority);

    if (!running_.load(std::memory_order_acquire)) {
        // No workers — run inline so futures still resolve. Lets tests/headless code submit
        // jobs without an explicit start().
        task();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        queues_[slot].emplace_back(std::move(task));
        pendingTotal_.fetch_add(1, std::memory_order_acq_rel);
    }
    cv_.notify_one();
}

bool JobSystem::popTask(Task& outTask)
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]() {
        if (stopping_.load(std::memory_order_acquire)) {
            return true;
        }
        for (const auto& q : queues_) {
            if (!q.empty()) {
                return true;
            }
        }
        return false;
    });

    for (auto& q : queues_) {
        if (!q.empty()) {
            outTask = std::move(q.front());
            q.pop_front();
            pendingTotal_.fetch_sub(1, std::memory_order_acq_rel);
            activeCount_.fetch_add(1, std::memory_order_acq_rel);
            return true;
        }
    }

    // Drain remaining work on shutdown so futures resolve.
    if (stopping_.load(std::memory_order_acquire)) {
        return false;
    }
    return false;
}

void JobSystem::workerLoop()
{
    while (true) {
        Task task;
        if (!popTask(task)) {
            // Stopping with empty queues.
            break;
        }

        try {
            task();
        } catch (...) {
            // Swallow; packaged_task forwards exceptions to the future.
        }

        activeCount_.fetch_sub(1, std::memory_order_acq_rel);
        if (pendingTotal_.load(std::memory_order_acquire) == 0
            && activeCount_.load(std::memory_order_acquire) == 0) {
            idleCv_.notify_all();
        }
    }
}

} // namespace voxel::core
