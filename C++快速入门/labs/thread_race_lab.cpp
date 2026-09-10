#include <thread>

int main() {
    int counter{0};
    std::thread first([&counter] {
        for (int index{0}; index < 10000; ++index) ++counter;
    });
    std::thread second([&counter] {
        for (int index{0}; index < 10000; ++index) ++counter;
    });
    first.join();
    second.join();
}
