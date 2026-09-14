#include "score.h"

#include <iostream>

int main() {
    const int rawScore{135};
    std::cout << "normalized: "
              << quickstart::normalizeScore(rawScore) << '\n';

#ifdef QUICKSTART_TRACE
    std::cout << "trace: raw=135, max=" << quickstart::kMaxScore << '\n';
#endif
}
