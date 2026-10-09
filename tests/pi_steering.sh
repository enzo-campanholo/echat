#!/bin/sh
set -eu
umask 077
task_dir=${TMPDIR:-/tmp}/uniichat-steering-$$
mkdir "$task_dir"
trap 'rm -rf "$task_dir"' 0 HUP INT TERM
./tests/pi_steering_test "$task_dir"
