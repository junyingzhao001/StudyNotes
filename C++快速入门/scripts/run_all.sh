#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
compiler="${CXX:-c++}"
flags=(-std=c++17 -Wall -Wextra -Wpedantic -g)
if [[ "${SANITIZE:-0}" == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
mkdir -p build
scratch_dir="$(mktemp -d)"
trap 'rm -rf "$scratch_dir"' EXIT

run_with_timeout() {
    local seconds="$1"
    shift
    local marker="$scratch_dir/timeout_$$_$RANDOM"
    "$@" &
    local command_pid=$!
    (
        sleep "$seconds"
        if kill -0 "$command_pid" 2>/dev/null; then
            : > "$marker"
            kill -TERM "$command_pid" 2>/dev/null || true
        fi
    ) &
    local watchdog_pid=$!
    local command_status=0
    wait "$command_pid" || command_status=$?
    kill "$watchdog_pid" 2>/dev/null || true
    wait "$watchdog_pid" 2>/dev/null || true
    if [[ -e "$marker" ]]; then
        echo "Timed out after ${seconds}s: $*" >&2
        return 124
    fi
    return "$command_status"
}

for source in demos/*.cpp; do
    number="$(basename "$source" .cpp)"
    binary="build/demo_$number"
    case "$number" in
        17|18|19|20|21)
            "$compiler" "${flags[@]}" -pthread "$source" -o "$binary"
            ;;
        *)
            "$compiler" "${flags[@]}" "$source" -o "$binary"
            ;;
    esac
    if [[ "$number" == 11 ]]; then
        if ! run_with_timeout 10 "$binary" "$scratch_dir/notes.txt" \
            > "$scratch_dir/output" 2> "$scratch_dir/stderr"; then
            cat "$scratch_dir/stderr"
            exit 1
        fi
    else
        if ! run_with_timeout 10 "$binary" \
            > "$scratch_dir/output" 2> "$scratch_dir/stderr"; then
            cat "$scratch_dir/stderr"
            exit 1
        fi
    fi
    diff -u "demos/$number.expected.txt" "$scratch_dir/output"
    [[ ! -s "$scratch_dir/stderr" ]] || { cat "$scratch_dir/stderr"; exit 1; }
    case "$number" in
        17|18|19|20|21)
            for repeat in 1 2 3 4; do
                if ! run_with_timeout 10 "$binary" \
                    > "$scratch_dir/repeated_output" 2> "$scratch_dir/stderr"; then
                    cat "$scratch_dir/stderr"
                    exit 1
                fi
                diff -u "demos/$number.expected.txt" "$scratch_dir/repeated_output"
                [[ ! -s "$scratch_dir/stderr" ]] || {
                    cat "$scratch_dir/stderr"
                    exit 1
                }
            done
            ;;
    esac
    echo "PASS demo $number"
done

expect_file_error() {
    local expected="$1"
    shift
    local file_error_exit=0
    build/demo_11 "$@" > "$scratch_dir/error" 2>&1 || file_error_exit=$?
    [[ "$file_error_exit" == 1 ]] || {
        echo "Unexpected exit status: $file_error_exit"
        exit 1
    }
    printf '%s\n' "$expected" > "$scratch_dir/expected_error"
    diff -u "$scratch_dir/expected_error" "$scratch_dir/error"
}
expect_file_error 'error: usage: demo NEW_OUTPUT_FILE'
expect_file_error 'error: file already exists' "$scratch_dir/notes.txt"
expect_file_error 'error: cannot open output' "$scratch_dir/missing/notes.txt"
diff -u <(printf 'C++17 file demo\n') "$scratch_dir/notes.txt"
echo 'PASS file error paths and overwrite protection'

"$compiler" "${flags[@]}" project/main.cpp project/task_store.cpp project/task_file.cpp \
    -o build/task_cli
for scenario in example errors; do
    build/task_cli < "project/$scenario.in" > "$scratch_dir/project_output" \
        2> "$scratch_dir/stderr"
    diff -u "project/$scenario.expected.txt" "$scratch_dir/project_output"
    [[ ! -s "$scratch_dir/stderr" ]] || { cat "$scratch_dir/stderr"; exit 1; }
    echo "PASS project $scenario"
done
"$compiler" "${flags[@]}" -Iproject tests/task_store_test.cpp project/task_store.cpp \
    -o build/task_store_test
build/task_store_test > "$scratch_dir/test_output" 2> "$scratch_dir/stderr"
diff -u <(printf 'ALL TESTS PASSED\n') "$scratch_dir/test_output"
[[ ! -s "$scratch_dir/stderr" ]] || { cat "$scratch_dir/stderr"; exit 1; }
echo 'PASS task_store unit tests'
"$compiler" "${flags[@]}" -Iproject tests/task_file_test.cpp \
    project/task_store.cpp project/task_file.cpp -o build/task_file_test
build/task_file_test > "$scratch_dir/test_output" 2> "$scratch_dir/stderr"
diff -u <(printf 'ALL TESTS PASSED\n') "$scratch_dir/test_output"
[[ ! -s "$scratch_dir/stderr" ]] || { cat "$scratch_dir/stderr"; exit 1; }
echo 'PASS task_file unit tests'

printf 'add Persistent task\nadd Remove me\ndone 1\nremove 2\nquit\n' | \
    build/task_cli "$scratch_dir/tasks.db" > "$scratch_dir/persist_first" \
        2> "$scratch_dir/stderr"
printf '%s\n' \
    'Commands: add TITLE | done ID | remove ID | list | quit' \
    'Added 1' \
    'Added 2' \
    'Completed.' \
    'Removed.' > "$scratch_dir/persist_first_expected"
diff -u "$scratch_dir/persist_first_expected" "$scratch_dir/persist_first"
[[ ! -s "$scratch_dir/stderr" ]] || { cat "$scratch_dir/stderr"; exit 1; }
printf 'list\nquit\n' | \
    build/task_cli "$scratch_dir/tasks.db" > "$scratch_dir/persist_second" \
        2> "$scratch_dir/stderr"
printf '%s\n' \
    'Commands: add TITLE | done ID | remove ID | list | quit' \
    '1 [x] Persistent task' > "$scratch_dir/persist_expected"
diff -u "$scratch_dir/persist_expected" "$scratch_dir/persist_second"
[[ ! -s "$scratch_dir/stderr" ]] || { cat "$scratch_dir/stderr"; exit 1; }
echo 'PASS project persistence across processes'

printf 'add Must not commit\nlist\nquit\n' | \
    build/task_cli "$scratch_dir/missing/tasks.db" > "$scratch_dir/save_failure" \
        2> "$scratch_dir/stderr"
printf '%s\n' \
    'Commands: add TITLE | done ID | remove ID | list | quit' \
    'Error: cannot open task file for writing' \
    'No tasks.' > "$scratch_dir/save_failure_expected"
diff -u "$scratch_dir/save_failure_expected" "$scratch_dir/save_failure"
[[ ! -s "$scratch_dir/stderr" ]] || { cat "$scratch_dir/stderr"; exit 1; }
echo 'PASS project save-failure rollback'

printf '1 maybe "bad"\n' > "$scratch_dir/corrupt.db"
corrupt_exit=0
build/task_cli "$scratch_dir/corrupt.db" > "$scratch_dir/corrupt_output" \
    2> "$scratch_dir/corrupt_error" || corrupt_exit=$?
[[ "$corrupt_exit" == 1 ]] || {
    echo "Unexpected corrupt-file status: $corrupt_exit"
    exit 1
}
[[ ! -s "$scratch_dir/corrupt_output" ]] || { cat "$scratch_dir/corrupt_output"; exit 1; }
diff -u <(printf 'Fatal: invalid task file\n') "$scratch_dir/corrupt_error"
echo 'PASS project corrupt-file handling'

"$compiler" "${flags[@]}" -pthread -Iconcurrency concurrency/main.cpp \
    concurrency/task_executor.cpp -o build/task_executor_demo
if ! run_with_timeout 10 build/task_executor_demo \
    > "$scratch_dir/executor_output" 2> "$scratch_dir/stderr"; then
    cat "$scratch_dir/stderr"
    exit 1
fi
diff -u concurrency/main.expected.txt "$scratch_dir/executor_output"
[[ ! -s "$scratch_dir/stderr" ]] || { cat "$scratch_dir/stderr"; exit 1; }
echo 'PASS task executor demo'

"$compiler" "${flags[@]}" -pthread -Iconcurrency \
    concurrency/task_executor_test.cpp concurrency/task_executor.cpp \
    -o build/task_executor_test
for repeat in 1 2 3 4 5; do
    if ! run_with_timeout 10 build/task_executor_test \
        > "$scratch_dir/executor_test_output" 2> "$scratch_dir/stderr"; then
        cat "$scratch_dir/stderr"
        exit 1
    fi
    diff -u <(printf 'ALL THREAD TESTS PASSED\n') \
        "$scratch_dir/executor_test_output"
    [[ ! -s "$scratch_dir/stderr" ]] || { cat "$scratch_dir/stderr"; exit 1; }
done
echo 'PASS task executor tests (5 runs)'
echo 'ALL CHECKS PASSED'
