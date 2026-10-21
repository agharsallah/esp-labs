#!/usr/bin/env sh
# Compiles and runs every host test (pure C++ on the Mac, no board). Run from the repo root.
# Each test's build line is the `//   c++ ...` comment at its top, so this script and the docs agree.
set -eu
cd "$(dirname "$0")/.."
out="${TMPDIR:-/tmp}/esp-labs-host-tests"
mkdir -p "$out"
failed=0
for test in kit/test/host/*.cpp labs/*/test/host/*.cpp; do
  [ -f "$test" ] || continue
  name=$(basename "$test" .cpp)
  includes=$(sed -n 's|^//   c++ -std=c++17 -Wall \(.*\) '"$test"' .*|\1|p' "$test")
  # shellcheck disable=SC2086 # the include flags are meant to split
  if c++ -std=c++17 -Wall $includes "$test" -o "$out/$name" && "$out/$name"; then
    echo "ok   $test"
  else
    echo "FAIL $test"
    failed=1
  fi
done
exit "$failed"
