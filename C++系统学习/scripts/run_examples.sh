#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

compiler="${CXX:-c++}"
flags=(-std=c++20 -Wall -Wextra -Wpedantic -g)
if [[ "${SANITIZE:-0}" == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
mkdir -p build
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

"$compiler" "${flags[@]}" examples/00_hello.cpp -o build/hello
diff -u <(printf 'C++ is ready\n') <(build/hello)

"$compiler" "${flags[@]}" examples/class_lifecycle.cpp -o build/class_lifecycle
diff -u <(printf 'before\nopen database\nuse database\nclose database\nafter\n') \
    <(build/class_lifecycle)

"$compiler" "${flags[@]}" examples/file_tasks.cpp -o build/file_tasks
diff -u <(printf '7: Learn C++\n') <(build/file_tasks "$scratch/tasks.txt")

"$compiler" "${flags[@]}" -pthread examples/thread_queue.cpp -o build/thread_queue
diff -u <(printf 'sum: 3\n') <(build/thread_queue)

"$compiler" "${flags[@]}" examples/frame_parser.cpp -o build/frame_parser
diff -u <(printf 'hello\nbye\n') <(build/frame_parser)

echo 'ALL EXAMPLES PASSED'
