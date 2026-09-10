#include "task_store.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace quickstart {

namespace {

bool isInvalidTitle(const std::string& text) {
    return text.find_first_not_of(" \t\r\n") == std::string::npos ||
           text.find_first_of("\r\n") != std::string::npos;
}

} // namespace

TaskStore::TaskStore(std::vector<Task> tasks) : tasks_(std::move(tasks)) {
    std::unordered_set<int> ids;
    int largestId{0};
    for (const auto& task : tasks_) {
        if (task.id <= 0 || task.id == std::numeric_limits<int>::max() ||
            isInvalidTitle(task.title) || !ids.insert(task.id).second) {
            throw std::invalid_argument("invalid stored task");
        }
        largestId = std::max(largestId, task.id);
    }
    nextId_ = largestId + 1;
}

int TaskStore::add(const std::string& title) {
    if (isInvalidTitle(title)) {
        throw std::invalid_argument("title must be a non-empty single line");
    }
    if (nextId_ == std::numeric_limits<int>::max()) {
        throw std::overflow_error("task id limit reached");
    }
    const int id = nextId_;
    tasks_.push_back(Task{id, title, false});
    ++nextId_; // 插入成功后才推进编号
    return id;
}

bool TaskStore::complete(int id) {
    const auto it = std::find_if(tasks_.begin(), tasks_.end(),
        [id](const Task& task) { return task.id == id; });
    if (it == tasks_.end()) return false;
    it->done = true;
    return true;
}

bool TaskStore::remove(int id) {
    const auto newEnd = std::remove_if(tasks_.begin(), tasks_.end(),
        [id](const Task& task) { return task.id == id; });
    if (newEnd == tasks_.end()) return false;
    tasks_.erase(newEnd, tasks_.end());
    return true;
}

const std::vector<Task>& TaskStore::tasks() const {
    return tasks_; // 借用内部容器，不复制
}

} // namespace quickstart
