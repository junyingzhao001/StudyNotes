#include <iostream>
#include <string>

int addCopy(int value) { return ++value; } // 返回修改后的副本
void addReference(int& value) { ++value; }
std::size_t length(const std::string& text) { return text.size(); }

int main() {
    int count{10};
    const int copiedResult = addCopy(count);
    std::cout << "after copy: " << count << '\n';
    std::cout << "returned copy: " << copiedResult << '\n';
    addReference(count);
    std::cout << "after reference: " << count << '\n';
    const std::string greeting{"hello"};
    std::cout << "length: " << length(greeting) << '\n';
}
