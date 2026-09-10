#ifndef CPP_QUICKSTART_TASK_FILE_H
#define CPP_QUICKSTART_TASK_FILE_H

#include "task_store.h"

#include <iosfwd>

namespace quickstart {

TaskStore loadTasks(std::istream& input);
void saveTasks(const TaskStore& store, std::ostream& output);

} // namespace quickstart

#endif
