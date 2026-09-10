#include <functional>
#include <iostream>
#include <thread>

void calculate(int input, int& output) {
    output = input * 2;
}

int main() {
    int result{0};
    std::thread worker(calculate, 21, std::ref(result));
    worker.join();

    std::cout << "result: " << result << '\n';
    std::cout << std::boolalpha
              << "joinable after join: " << worker.joinable() << '\n';
}
