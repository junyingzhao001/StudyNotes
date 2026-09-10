#include <algorithm>
#include <iostream>
#include <vector>

int main() {
    std::vector<int> scores{90, 40, 70, 60};
    int threshold{60}; // 可配置阈值，由 Lambda 复制捕获
    const auto newEnd = std::remove_if(scores.begin(), scores.end(),
        [threshold](int score) { return score < threshold; });
    scores.erase(newEnd, scores.end());
    std::sort(scores.begin(), scores.end(),
        [](int left, int right) { return left > right; });
    for (int score : scores) std::cout << score << ' ';
    std::cout << '\n';
    const auto it = std::find_if(scores.begin(), scores.end(),
        [](int score) { return score >= 80; });
    if (it != scores.end()) std::cout << "first >= 80: " << *it << '\n';
}
