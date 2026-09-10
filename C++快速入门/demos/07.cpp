#include <iostream>
#include <map>
#include <string>
#include <vector>

int main() {
    std::vector<std::string> words{"cpp", "java", "cpp"};
    words.push_back("rust");
    std::map<std::string, int> counts;
    for (const auto& word : words) ++counts[word];
    for (const auto& [word, count] : counts) {
        std::cout << word << ": " << count << '\n';
    }
    const auto it = counts.find("python");
    std::cout << "python found: " << (it != counts.end()) << '\n';
    std::cout << "first: " << words.at(0) << '\n';
}
