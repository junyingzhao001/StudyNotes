#include "task_file.h"
#include "task_store.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace {

void printTasks(const quickstart::TaskStore& store) {
    if (store.tasks().empty()) std::cout << "No tasks.\n";
    for (const auto& task : store.tasks()) {
        std::cout << task.id << " [" << (task.done ? 'x' : ' ')
                  << "] " << task.title << '\n';
    }
}

void saveIfRequested(const quickstart::TaskStore& store,
                     const std::filesystem::path* path) {
    if (path == nullptr) return;

    auto temporaryPath = *path;
    temporaryPath += ".tmp";
    std::ofstream output(temporaryPath, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open task file for writing");
    try {
        quickstart::saveTasks(store, output);
        output.close();
        if (!output) throw std::runtime_error("cannot finish writing task file");
    } catch (...) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        throw;
    }

    std::error_code renameError;
    std::filesystem::rename(temporaryPath, *path, renameError);
    if (renameError) {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        throw std::runtime_error("cannot replace task file");
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc > 2) {
        std::cerr << "Usage: task_cli [DATA_FILE]\n";
        return 1;
    }

    try {
        const std::filesystem::path dataPath = argc == 2 ? argv[1] : "";
        const std::filesystem::path* path = argc == 2 ? &dataPath : nullptr;
        quickstart::TaskStore store;
        if (path != nullptr && std::filesystem::exists(*path)) {
            std::ifstream input(*path);
            if (!input) throw std::runtime_error("cannot open task file for reading");
            store = quickstart::loadTasks(input);
        }

        std::cout << "Commands: add TITLE | done ID | remove ID | list | quit\n";
        std::string line;
        while (std::getline(std::cin, line)) {
            std::istringstream input(line);
            std::string command;
            if (!(input >> command)) continue;

            try {
                if (command == "add") {
                    std::string title;
                    std::getline(input >> std::ws, title);
                    auto updated = store;
                    const int id = updated.add(title);
                    saveIfRequested(updated, path);
                    store = std::move(updated);
                    std::cout << "Added " << id << '\n';
                } else if (command == "done" || command == "remove") {
                    int id{0};
                    std::string extra;
                    if (!(input >> id) || id <= 0 || (input >> extra)) {
                        std::cout << "Invalid id.\n";
                        continue;
                    }
                    auto updated = store;
                    const bool changed = command == "done"
                        ? updated.complete(id)
                        : updated.remove(id);
                    if (changed) {
                        saveIfRequested(updated, path);
                        store = std::move(updated);
                    }
                    std::cout << (changed
                        ? (command == "done" ? "Completed.\n" : "Removed.\n")
                        : "Not found.\n");
                } else if (command == "list" || command == "quit") {
                    std::string extra;
                    if (input >> extra) {
                        std::cout << "Unexpected argument.\n";
                        continue;
                    }
                    if (command == "quit") break;
                    printTasks(store);
                } else {
                    std::cout << "Unknown command.\n";
                }
            } catch (const std::exception& error) {
                std::cout << "Error: " << error.what() << '\n';
            }
        }

        if (std::cin.bad() || (std::cin.fail() && !std::cin.eof())) {
            std::cerr << "Input read failed.\n";
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "Fatal: " << error.what() << '\n';
        return 1;
    }
}
