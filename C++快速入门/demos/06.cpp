#include <iostream>
#include <memory>
#include <string>
#include <utility>

struct Task {
    explicit Task(const std::string& value) : title(value) {}
    std::string title;
};

void printTask(const Task* task) { // 仅借用
    if (task) std::cout << task->title << '\n';
}

int main() {
    auto owner = std::make_unique<Task>("Learn ownership");
    printTask(owner.get());
    auto nextOwner = std::move(owner);
    std::cout << std::boolalpha << "old owner empty: " << (owner == nullptr) << '\n';
    printTask(nextOwner.get());
}
