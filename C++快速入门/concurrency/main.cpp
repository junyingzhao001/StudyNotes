#include "task_executor.h"

#include <exception>
#include <iostream>
#include <stdexcept>

int main() {
    quickstart::TaskExecutor executor;
    auto square = executor.submit([] { return 12 * 12; });
    auto failure = executor.submit([]() -> int {
        throw std::runtime_error("task failed");
    });

    std::cout << "square: " << square.get() << '\n';
    try {
        std::cout << failure.get() << '\n';
    } catch (const std::exception& error) {
        std::cout << "error: " << error.what() << '\n';
    }
}
