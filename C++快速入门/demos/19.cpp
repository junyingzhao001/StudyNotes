#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>

int main() {
    std::queue<int> jobs;
    std::mutex mutex;
    std::condition_variable ready;
    bool finished{false};
    int sum{0};

    std::thread producer([&] {
        for (int value{1}; value <= 5; ++value) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                jobs.push(value);
            }
            ready.notify_one();
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = true;
        }
        ready.notify_all();
    });

    std::thread consumer([&] {
        while (true) {
            std::unique_lock<std::mutex> lock(mutex);
            ready.wait(lock, [&] { return finished || !jobs.empty(); });
            if (jobs.empty() && finished) break;
            const int value = jobs.front();
            jobs.pop();
            lock.unlock();
            sum += value;
        }
    });

    producer.join();
    consumer.join();
    std::cout << "sum: " << sum << '\n';
}
