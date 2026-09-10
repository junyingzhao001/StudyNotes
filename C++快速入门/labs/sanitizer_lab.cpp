#include <iostream>
#include <vector>

int main() {
    const std::vector<int> scores{80, 90, 100};
    // 故意越界：仅用于配合 AddressSanitizer 学习错误报告。
    std::cout << scores[3] << '\n';
}
