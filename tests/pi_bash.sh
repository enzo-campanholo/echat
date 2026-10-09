#!/bin/sh
set -eu
umask 077
task_dir=${TMPDIR:-/tmp}/uniichat-bash-$$
mkdir "$task_dir"
trap 'rm -rf "$task_dir"' 0 HUP INT TERM
./tests/pi_bash_test "$task_dir"
