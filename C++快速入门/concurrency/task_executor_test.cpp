#include "task_executor.h"

#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures{0};

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void testResultAndException() {
    quickstart::TaskExecutor executor;
    auto answer = executor.submit([] { return 40 + 2; });
    auto failure = executor.submit([]() -> int {
        throw std::runtime_error("expected failure");
    });
    expect(answer.get() == 42, "future should contain task result");
    try {
        static_cast<void>(failure.get());
        expect(false, "future.get should rethrow task exception");
    } catch (const std::runtime_error& error) {
        expect(std::string{error.what()} == "expected failure",
               "future should preserve exception message");
    }
    auto afterFailure = executor.submit([] { return 7; });
    expect(afterFailure.get() == 7, "worker should continue after task exception");
}

void testFifoAndShutdown() {
    quickstart::TaskExecutor executor;
    std::vector<int> order;
    std::vector<std::future<void>> results;
    for (int value{1}; value <= 3; ++value) {
        results.push_back(executor.submit([&order, value] { order.push_back(value); }));
    }
    for (auto& result : results) result.get();
    expect(order == std::vector<int>({1, 2, 3}),
           "single worker should start tasks in FIFO order");

    executor.shutdown();
    executor.shutdown(); // 重复关闭应安全
    bool threw = false;
    try {
        static_cast<void>(executor.submit([] {}));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "submit after shutdown should be rejected");
}

void testDestructorDrainsQueue() {
    std::atomic<int> completed{0};
    {
        quickstart::TaskExecutor executor;
        for (int index{0}; index < 100; ++index) {
            executor.submit([&completed] { ++completed; });
        }
    }
    expect(completed.load() == 100, "destructor should finish queued tasks");
}

void testConcurrentProducers() {
    quickstart::TaskExecutor executor;
    std::atomic<int> completed{0};
    std::vector<std::future<void>> producers;
    for (int producer{0}; producer < 4; ++producer) {
        producers.push_back(std::async(std::launch::async,
            [&executor, &completed] {
                std::vector<std::future<void>> results;
                for (int task{0}; task < 25; ++task) {
                    results.push_back(executor.submit([&completed] { ++completed; }));
                }
                for (auto& result : results) result.get();
            }));
    }
    for (auto& producer : producers) producer.get();
    expect(completed.load() == 100,
           "concurrent producers should submit every task exactly once");
}

void testConcurrentShutdownDrainsQueue() {
    quickstart::TaskExecutor executor;
    std::atomic<int> completed{0};
    std::promise<void> firstStarted;
    auto started = firstStarted.get_future();
    std::promise<void> releaseFirst;
    auto firstGate = releaseFirst.get_future().share();

    auto first = executor.submit([&] {
        firstStarted.set_value();
        firstGate.wait();
        ++completed;
    });
    std::vector<std::future<void>> queued;
    for (int task{0}; task < 20; ++task) {
        queued.push_back(executor.submit([&completed] { ++completed; }));
    }
    started.wait();

    std::promise<void> startShutdown;
    auto shutdownGate = startShutdown.get_future().share();
    std::atomic<int> shutdownCallersReady{0};
    auto close = [&] {
        ++shutdownCallersReady;
        shutdownGate.wait();
        executor.shutdown();
    };

    std::future<void> firstShutdown;
    std::future<void> secondShutdown;
    try {
        firstShutdown = std::async(std::launch::async, close);
        secondShutdown = std::async(std::launch::async, close);
    } catch (...) {
        startShutdown.set_value();
        releaseFirst.set_value();
        if (firstShutdown.valid()) firstShutdown.get();
        throw;
    }

    while (shutdownCallersReady.load() != 2) std::this_thread::yield();
    startShutdown.set_value();

    // submit 与 shutdown 允许并发；等到拒绝即证明 stopping_ 已在锁内设定。
    std::atomic<int> probeCompleted{0};
    int probeAccepted{0};
    std::vector<std::future<void>> probeResults;
    while (true) {
        try {
            probeResults.push_back(
                executor.submit([&probeCompleted] { ++probeCompleted; }));
            ++probeAccepted;
            std::this_thread::yield();
        } catch (const std::runtime_error&) {
            break;
        }
    }
    releaseFirst.set_value();

    firstShutdown.get();
    secondShutdown.get();
    first.get();
    for (auto& result : queued) result.get();
    for (auto& result : probeResults) result.get();
    expect(completed.load() == 21,
           "concurrent shutdown should drain the tasks queued before shutdown");
    expect(probeCompleted.load() == probeAccepted,
           "shutdown should drain every task accepted during the submit race");
}

void testWorkerCannotShutdownItself() {
    quickstart::TaskExecutor executor;
    auto selfShutdown = executor.submit([&executor] { executor.shutdown(); });
    try {
        selfShutdown.get();
        expect(false, "worker shutdown should throw instead of joining itself");
    } catch (const std::logic_error& error) {
        expect(std::string{error.what()} == "executor cannot stop itself",
               "worker shutdown should report a clear logic error");
    }

    auto stillRunning = executor.submit([] { return 9; });
    expect(stillRunning.get() == 9,
           "rejected self-shutdown should leave executor running");
}

} // namespace

int main() {
    testResultAndException();
    testFifoAndShutdown();
    testConcurrentProducers();
    testConcurrentShutdownDrainsQueue();
    testDestructorDrainsQueue();
    testWorkerCannotShutdownItself();
    if (failures != 0) {
        std::cerr << failures << " TEST(S) FAILED\n";
        return 1;
    }
    std::cout << "ALL THREAD TESTS PASSED\n";
}
