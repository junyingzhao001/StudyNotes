#include "task_executor.h"

#include <exception>
#include <utility>

namespace {

thread_local const quickstart::TaskExecutor* runningExecutor{nullptr};

} // namespace

namespace quickstart {

TaskExecutor::TaskExecutor() : worker_(&TaskExecutor::run, this) {}

TaskExecutor::~TaskExecutor() noexcept {
    try {
        shutdown();
    } catch (...) {
        std::terminate();
    }
}

void TaskExecutor::shutdown() {
    // 必须先判断：外部线程可能正持有 shutdownMutex_ 并等待本 worker 结束。
    if (runningExecutor == this) {
        throw std::logic_error("executor cannot stop itself");
    }
    std::lock_guard<std::mutex> shutdownLock(shutdownMutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void TaskExecutor::run() {
    runningExecutor = this;
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) {
                runningExecutor = nullptr;
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task(); // packaged_task 会把返回值或异常保存到 future
    }
}

} // namespace quickstart
