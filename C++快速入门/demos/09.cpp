#include <iostream>
#include <string>
#include <utility>
#include <vector>

std::vector<std::string> makeNames() {
    std::vector<std::string> result{"Ada", "Bjarne"};
    return result;
}

int main() {
    auto original = makeNames();
    auto copied = original;
    copied.at(0) = "Grace";
    auto moved = std::move(original);
    std::cout << "copy: " << copied.at(0) << '\n';
    std::cout << "moved: " << moved.at(0) << '\n';
    original = {"New value"}; // 重新赋值后再使用
    std::cout << "reused: " << original.at(0) << '\n';
}
