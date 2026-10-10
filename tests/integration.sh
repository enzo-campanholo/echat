#!/bin/sh
set -eu
umask 077
task_dir=${TMPDIR:-/tmp}/uniichat-test-$$
mkdir "$task_dir"
trap 'rm -rf "$task_dir"' 0 HUP INT TERM
./tests/server_test "$task_dir/chat"
# End of input: every complete request already sent is answered and logged.
printf '%s\n' '{"id":1,"op":"append","kind":"note","text":"one"}' \
    '{"id":2,"op":"append","records":[{"kind":"note","text":"two"},{"kind":"note","text":"three"}]}' \
    '{"id":3,"op":"status"}' | ./unii serve "$task_dir/eof" > "$task_dir/eof.out"
test "$(sed -n '1p' "$task_dir/eof.out")" = '{"id":1,"ok":true,"message_id":0}'
test "$(sed -n '2p' "$task_dir/eof.out")" = '{"id":2,"ok":true,"message_id":1}'
sed -n '3p' "$task_dir/eof.out" | grep -q '^{"id":3,"ok":true,"messages":3,'
test "$(wc -l < "$task_dir/eof.out")" -eq 3
test "$(./unii log "$task_dir/eof" 0 | wc -l)" -eq 3
# A shutdown request still ends dispatching.
printf '%s\n' '{"id":4,"op":"append","kind":"note","text":"four"}' '{"id":5,"op":"shutdown"}' \
    '{"id":6,"op":"append","kind":"note","text":"never"}' | ./unii serve "$task_dir/eof" > "$task_dir/eof.out"
test "$(cat "$task_dir/eof.out")" = '{"id":4,"ok":true,"message_id":3}
{"id":5,"ok":true}'
test "$(./unii log "$task_dir/eof" 0 | wc -l)" -eq 4
printf 'Imported verbatim.\n' | ./unii add "$task_dir/import" note
./unii zoom "$task_dir/import" 0 1 > "$task_dir/zoom"
test "$(sed -n '2p' "$task_dir/zoom")" = 'Imported verbatim.'
./unii date "$task_dir/import" 0 > "$task_dir/date"
./unii view "$task_dir/import" > "$task_dir/view"
./unii log "$task_dir/import" 0 > "$task_dir/log"
grep -q '^{"i":0,"kind":"note","text":"Imported verbatim.\\n",' "$task_dir/log"
# C2 rewords one sentence of the spec prompt while TAIL_BYTES is nonzero.
./unii prompt > "$task_dir/prompt"
grep -q "^not summarized yet follow the view whole, inside <recent> tags." "$task_dir/prompt"
! grep -q "shows as \"(not summarized yet" "$task_dir/prompt"
if ./unii add "$task_dir/import" thought </dev/null 2>/dev/null; then
    echo 'thoughts must not be logged' >&2
    exit 1
fi
# Readers never create a chat.
typo="$task_dir/typo"
for args in "view $typo" "status $typo" "zoom $typo 0 1" "date $typo 0" "log $typo 0"; do
    if ./unii $args >/dev/null 2>&1; then
        echo "unii $args opened a missing chat" >&2
        exit 1
    fi
done
test ! -e "$typo"
printf 'integration: piped requests at end of input, CLI import, zoom, date, log, read-only commands, thought rejection OK\n'
