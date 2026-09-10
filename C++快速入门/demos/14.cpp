#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

std::int64_t sum(const int* data, std::size_t size) {
    std::int64_t total{0};
    for (std::size_t index{0}; index < size; ++index) {
        total += data[index];
    }
    return total;
}

void printText(std::string_view text) { // 只读借用，不拥有字符
    std::cout << text << " (" << text.size() << " bytes)\n";
}

int main() {
    const std::array<int, 3> scores{80, 90, 100};
    std::cout << "sum: " << sum(scores.data(), scores.size()) << '\n';

    const std::string language{"C++17"};
    printText(language);
    const char* cText = language.c_str();
    std::cout << "C API view: " << cText << '\n';
}
