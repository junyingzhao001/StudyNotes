#include "task_file.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

int failures{0};

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void testRoundTripPreservesDataAndNextId() {
    quickstart::TaskStore original;
    const std::string specialTitle{"quoted \"title\" with \\ slash"};
    original.add(specialTitle);
    const int removedId = original.add("remove me");
    const int thirdId = original.add("keep me");
    original.complete(thirdId);
    original.remove(removedId);

    std::stringstream data;
    quickstart::saveTasks(original, data);
    auto loaded = quickstart::loadTasks(data);

    expect(loaded.tasks().size() == 2, "two tasks should survive round trip");
    expect(loaded.tasks().at(0).title == specialTitle,
           "quotes and backslashes should survive round trip");
    expect(loaded.tasks().at(1).id == 3, "stored id gap should be preserved");
    expect(loaded.tasks().at(1).done, "completed flag should be preserved");
    expect(loaded.add("next") == 4, "next id should follow largest stored id");
}

void testRejectsCorruptData() {
    std::istringstream data{"1 maybe \"bad\"\n"};
    bool threw = false;
    try {
        static_cast<void>(quickstart::loadTasks(data));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "corrupt row should throw runtime_error");

    std::istringstream unquoted{"1 0 title\n"};
    threw = false;
    try {
        static_cast<void>(quickstart::loadTasks(unquoted));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "unquoted title should be rejected");

    std::istringstream failed;
    failed.setstate(std::ios::failbit);
    threw = false;
    try {
        static_cast<void>(quickstart::loadTasks(failed));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "failed input stream should be rejected");
}

} // namespace

int main() {
    testRoundTripPreservesDataAndNextId();
    testRejectsCorruptData();
    if (failures != 0) {
        std::cerr << failures << " TEST(S) FAILED\n";
        return 1;
    }
    std::cout << "ALL TESTS PASSED\n";
}
