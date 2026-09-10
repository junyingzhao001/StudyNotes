#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

class JoiningThreads {
public:
    explicit JoiningThreads(std::vector<std::thread>& threads) : threads_(threads) {}
    ~JoiningThreads() {
        for (auto& thread : threads_) {
            if (thread.joinable()) thread.join();
        }
    }

    JoiningThreads(const JoiningThreads&) = delete;
    JoiningThreads& operator=(const JoiningThreads&) = delete;

private:
    std::vector<std::thread>& threads_;
};

void increment(int& counter, std::mutex& mutex, int times) {
    for (int index{0}; index < times; ++index) {
        std::lock_guard<std::mutex> lock(mutex);
        ++counter;
    }
}

int main() {
    int counter{0};
    std::mutex mutex;
    {
        std::vector<std::thread> workers;
        JoiningThreads joining{workers};
        for (int index{0}; index < 4; ++index) {
            workers.emplace_back(increment, std::ref(counter), std::ref(mutex),
                                 5000);
        }
    }
    std::cout << "counter: " << counter << '\n';
}
