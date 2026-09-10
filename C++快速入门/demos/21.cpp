#include <atomic>
#include <iostream>
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

int main() {
    std::atomic<int> counter{0};
    {
        std::vector<std::thread> workers;
        JoiningThreads joining{workers};
        for (int index{0}; index < 4; ++index) {
            workers.emplace_back([&counter] {
                for (int repeat{0}; repeat < 5000; ++repeat) ++counter;
            });
        }
    }
    std::cout << "counter: " << counter.load() << '\n';
}
