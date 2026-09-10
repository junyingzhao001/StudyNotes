#include "task_store.h"

#include <iostream>
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

void testAddAndComplete() {
    quickstart::TaskStore store;
    const int firstId = store.add("Learn references");
    const int secondId = store.add("Build a CLI");

    expect(firstId == 1 && secondId == 2, "ids should increase from 1");
    expect(store.tasks().size() == 2, "two tasks should be stored");
    expect(store.complete(firstId), "existing task should be completed");
    expect(store.tasks().at(0).done, "completed state should be visible");
    expect(!store.complete(99), "missing task should return false");
    expect(store.remove(secondId), "existing task should be removed");
    expect(store.tasks().size() == 1, "one task should remain after remove");
    expect(!store.remove(99), "removing missing task should return false");
}

void testRejectsInvalidTitleWithoutUsingId() {
    quickstart::TaskStore store;
    bool threw = false;
    try {
        store.add("   \t");
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    expect(threw, "blank title should throw invalid_argument");
    try {
        store.add("line one\nline two");
        expect(false, "multiline title should throw invalid_argument");
    } catch (const std::invalid_argument&) {
        // expected
    }
    expect(store.add("Valid") == 1, "failed add should not consume an id");
}

} // namespace

int main() {
    testAddAndComplete();
    testRejectsInvalidTitleWithoutUsingId();
    if (failures != 0) {
        std::cerr << failures << " TEST(S) FAILED\n";
        return 1;
    }
    std::cout << "ALL TESTS PASSED\n";
}
