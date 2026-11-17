#!/bin/sh
# tools/ci/platform.sh <os>: zuslothdb's per-OS checks, run once on each
# runner of platforms.yaml (os is macos, windows or ubuntu), with
# R_ACTIONS_PROFILE set to quick or full. Every check is a step here, in
# order; set -e names the one that failed. Run from the package root; it
# also runs locally.
#
# No steps yet. Stage 1 adds upstream's doctest suite on the patched tree
# (tools/upstream-tests.sh) and the check that the shared library exports
# only R_init_zuslothdb. Slow extras go under R_ACTIONS_PROFILE=full.
set -eu
os=${1:?usage: tools/ci/platform.sh <macos|windows|ubuntu>}
case "$os" in
  macos | windows | ubuntu) ;;
  *) echo "unknown os: $os" >&2; exit 2 ;;
esac
echo "platform.sh: $os (profile ${R_ACTIONS_PROFILE:-quick}), no steps yet"
