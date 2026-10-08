#!/bin/sh
set -eu
exec pi "$@" --extension "$UNII_TEST_WORKER_PROVIDER"
