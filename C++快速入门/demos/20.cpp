#include <future>
#include <iostream>
#include <stdexcept>

int divide(int left, int right) {
    if (right == 0) throw std::invalid_argument("division by zero");
    return left / right;
}

int main() {
    auto answer = std::async(std::launch::async, divide, 84, 2);
    auto failure = std::async(std::launch::async, divide, 10, 0);

    std::cout << "answer: " << answer.get() << '\n';
    try {
        std::cout << failure.get() << '\n';
    } catch (const std::exception& error) {
        std::cout << "error: " << error.what() << '\n';
    }
}
