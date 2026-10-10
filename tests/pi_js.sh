#!/bin/sh
set -eu
umask 077
task_dir=${TMPDIR:-/tmp}/uniichat-js-$$
mkdir "$task_dir"
trap 'rm -rf "$task_dir"' 0 HUP INT TERM
PI_CODING_AGENT_DIR="$task_dir/pi-config" PI_OFFLINE=1 \
    pi --mode rpc --no-session --offline --no-extensions --no-skills \
    --no-prompt-templates --no-context-files --no-mcp \
    --extension "$(pwd)/tests/pi_js_test.ts" </dev/null 2>"$task_dir/err" >/dev/null || {
    cat "$task_dir/err" >&2
    exit 1
}
grep -q '^pi_js_test: ok$' "$task_dir/err"
printf 'pi JS adapter: view blocks, cache marks, previous-turn breakpoint, well-formed requests, calls bound to their engine OK\n'
