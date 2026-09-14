#include "score.h"

namespace {

int clampAtLeastZero(int score) {
    return score < 0 ? 0 : score;
}

} // namespace

namespace quickstart {

int normalizeScore(int score) {
    const int nonnegative = clampAtLeastZero(score);
    return nonnegative > kMaxScore ? kMaxScore : nonnegative;
}

} // namespace quickstart
