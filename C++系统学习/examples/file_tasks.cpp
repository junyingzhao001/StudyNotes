#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: file_tasks OUTPUT\n";
        return 2;
    }

    const std::string path{argv[1]};
    {
        std::ofstream out{path};
        if (!out) throw std::runtime_error{"cannot open output"};
        out << 7 << ' ' << std::quoted("Learn C++") << '\n';
        out.close();
        if (!out) throw std::runtime_error{"write failed"};
    }

    std::ifstream input{path};
    int id{};
    std::string title;
    if (!(input >> id >> std::quoted(title))) {
        throw std::runtime_error{"invalid task file"};
    }
    std::cout << id << ": " << title << '\n';
}
