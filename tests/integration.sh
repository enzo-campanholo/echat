#!/bin/sh
set -eu
umask 077
task_dir=${TMPDIR:-/tmp}/uniichat-test-$$
mkdir "$task_dir"
trap 'rm -rf "$task_dir"' 0 HUP INT TERM
./tests/server_test "$task_dir/chat"
printf 'Imported verbatim.\n' | ./unii add "$task_dir/import" note
./unii zoom "$task_dir/import" 0 1 > "$task_dir/zoom"
test "$(sed -n '2p' "$task_dir/zoom")" = 'Imported verbatim.'
./unii date "$task_dir/import" 0 > "$task_dir/date"
./unii view "$task_dir/import" > "$task_dir/view"
if ./unii add "$task_dir/import" thought </dev/null 2>/dev/null; then
    echo 'thoughts must not be logged' >&2
    exit 1
fi
printf 'integration: CLI import, zoom, date, thought rejection OK\n'
