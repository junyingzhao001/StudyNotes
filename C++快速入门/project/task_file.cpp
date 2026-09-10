#include "task_file.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace quickstart {

TaskStore loadTasks(std::istream& input) {
    std::vector<Task> tasks;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::istringstream row(line);
        int id{0};
        int done{0};
        std::string title;
        std::string extra;
        if (!(row >> id >> done)) throw std::runtime_error("invalid task file");
        row >> std::ws;
        if (row.peek() != '"' || !(row >> std::quoted(title)) ||
            (done != 0 && done != 1) || (row >> extra)) {
            throw std::runtime_error("invalid task file");
        }
        tasks.push_back(Task{id, std::move(title), done == 1});
    }
    if (input.bad() || (input.fail() && !input.eof())) {
        throw std::runtime_error("cannot read task file");
    }
    try {
        return TaskStore{std::move(tasks)};
    } catch (const std::invalid_argument&) {
        throw std::runtime_error("invalid task file");
    }
}

void saveTasks(const TaskStore& store, std::ostream& output) {
    for (const auto& task : store.tasks()) {
        output << task.id << ' ' << (task.done ? 1 : 0) << ' '
               << std::quoted(task.title) << '\n';
    }
    output.flush();
    if (!output) throw std::runtime_error("cannot write task file");
}

} // namespace quickstart
