#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

std::optional<int> firstScore(bool available) {
    if (!available) return std::nullopt;
    return 80;
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) throw std::invalid_argument("usage: demo NEW_OUTPUT_FILE");
        const std::filesystem::path path{argv[1]};
        if (std::filesystem::exists(path)) throw std::runtime_error("file already exists");
        {
            std::ofstream output(path);
            if (!output) throw std::runtime_error("cannot open output");
            output << "C++17 file demo" << '\n';
            output.close();
            if (!output) throw std::runtime_error("cannot finish writing");
        }
        std::ifstream input(path);
        if (!input) throw std::runtime_error("cannot open input");
        std::string line;
        while (std::getline(input, line)) std::cout << line << '\n';
        if (!input.eof() || input.bad()) throw std::runtime_error("cannot finish reading");
        const auto score = firstScore(false);
        std::cout << "score: " << score.value_or(0) << '\n';
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
