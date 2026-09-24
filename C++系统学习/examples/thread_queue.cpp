#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>

class TaskQueue {
public:
    TaskQueue() : worker_{[this] { run(); }} {}

    ~TaskQueue() {
        {
            std::lock_guard lock{mutex_};
            stopping_ = true;
        }
        ready_.notify_one();
        worker_.join();
    }

    void submit(std::function<void()> task) {
        {
            std::lock_guard lock{mutex_};
            tasks_.push(std::move(task));
        }
        ready_.notify_one();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock{mutex_};
                ready_.wait(lock, [this] {
                    return stopping_ || !tasks_.empty();
                });
                if (stopping_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<std::function<void()>> tasks_;
    bool stopping_{false};
    std::thread worker_;
};

int main() {
    int sum{};
    std::mutex sumMutex;
    {
        TaskQueue queue;
        queue.submit([&] {
            std::lock_guard lock{sumMutex};
            sum += 1;
        });
        queue.submit([&] {
            std::lock_guard lock{sumMutex};
            sum += 2;
        });
    }
    std::cout << "sum: " << sum << '\n';
}
