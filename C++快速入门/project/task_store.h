#ifndef CPP_QUICKSTART_TASK_STORE_H
#define CPP_QUICKSTART_TASK_STORE_H

#include <string>
#include <vector>

namespace quickstart {

struct Task {
    int id;
    std::string title;
    bool done{false};
};

class TaskStore {
public:
    TaskStore() = default;
    explicit TaskStore(std::vector<Task> tasks);
    int add(const std::string& title);
    bool complete(int id);
    bool remove(int id);
    const std::vector<Task>& tasks() const;
private:
    int nextId_{1};
    std::vector<Task> tasks_;
};

} // namespace quickstart

#endif
