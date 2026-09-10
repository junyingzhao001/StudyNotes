#ifndef CPP_QUICKSTART_TASK_EXECUTOR_H
#define CPP_QUICKSTART_TASK_EXECUTOR_H

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace quickstart {

class TaskExecutor {
public:
    TaskExecutor();
    ~TaskExecutor() noexcept;

    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;
    TaskExecutor(TaskExecutor&&) = delete;
    TaskExecutor& operator=(TaskExecutor&&) = delete;

    void shutdown();

    template<typename Function>
    auto submit(Function&& function)
        -> std::future<std::invoke_result_t<std::decay_t<Function>&>> {
        using Result = std::invoke_result_t<std::decay_t<Function>&>;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::forward<Function>(function));
        auto result = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) throw std::runtime_error("executor is stopping");
            tasks_.push([task] { (*task)(); });
        }
        ready_.notify_one();
        return result;
    }

private:
    void run();

    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<std::function<void()>> tasks_;
    bool stopping_{false};
    std::mutex shutdownMutex_;
    std::thread worker_; // 最后构造，确保前面的同步对象已经存在
};

} // namespace quickstart

#endif
