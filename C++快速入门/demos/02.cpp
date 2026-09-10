#include <iostream>
#include <string>
#include <vector>

enum class Level { Beginner, Advanced };

int main() {
    const std::string language{"C++"};
    std::vector<int> scores{80, 90, 100};
    auto sum = 0;
    for (int score : scores) {
        sum += score;
    }
    const double average = static_cast<double>(sum) / scores.size();
    const auto level = Level::Beginner;
    if (level == Level::Beginner && average >= 60) {
        std::cout << language << " average: " << average << '\n';
    }
}
