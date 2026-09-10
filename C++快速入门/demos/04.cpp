#include <iostream>

void printIfPresent(const int* value) {
    if (value != nullptr) {
        std::cout << "value: " << *value << '\n';
    } else {
        std::cout << "no value" << '\n';
    }
}

int main() {
    int score{80};
    int* observer = &score; // 借用 score，不负责释放
    *observer = 95;
    printIfPresent(observer);
    printIfPresent(nullptr);
    // observer 和 score 的使用都在 score 的生命周期内。
}
