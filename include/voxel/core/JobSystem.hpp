#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace voxel::core {

enum class JobPriority {
    Critical,
    High,
    Medium,
    Low
};

struct JobDescription {
    std::string name;
    JobPriority priority{JobPriority::Medium};
};

class JobSystem {
public:
    JobSystem();
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    void start(std::size_t workerCount = 0);
    void stop();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::size_t workerCount() const noexcept;
    [[nodiscard]] std::size_t pendingCount() const noexcept;

    // Block until all currently enqueued and in-flight jobs have completed.
    // MUST NOT be called from inside a worker thread (would deadlock); asserted at runtime.
    void waitAll();

    template <typename Fn>
    auto submit(JobDescription description, Fn&& fn) -> std::future<std::invoke_result_t<Fn>>
    {
        using Result = std::invoke_result_t<Fn>;
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(fn));
        std::future<Result> future = task->get_future();
        enqueue(description.priority, [task]() { (*task)(); });
        return future;
    }

private:
    using Task = std::function<void()>;

    // Follow-up seam: keep the public submit API narrow so we can split this
    // implementation into long-running world workers, short upload/reduction
    // workers, and IO/cache workers without changing call sites first.
    void enqueue(JobPriority priority, Task task);
    void workerLoop();
    bool popTask(Task& outTask);

    static constexpr std::size_t kPriorityCount = 4;

    std::vector<std::thread> workers_;
    std::deque<Task> queues_[kPriorityCount];

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable idleCv_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<std::size_t> activeCount_{0};
    std::atomic<std::size_t> pendingTotal_{0};
};

} // namespace voxel::core
