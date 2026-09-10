#include <cstddef>
#include <iostream>
#include <vector>

std::size_t firstAtLeast(const std::vector<int>& scores, int threshold) {
    for (std::size_t index{0}; index < scores.size(); ++index) {
        if (scores[index] > threshold) {
            return index;
        }
    }
    return scores.size();
}

int main() {
    const std::vector<int> scores{60, 90};
    const auto index = firstAtLeast(scores, 90);
    if (index == scores.size()) {
        std::cout << "not found\n";
    } else {
        std::cout << "found index: " << index << '\n';
    }
}
