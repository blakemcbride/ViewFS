#!/usr/bin/env bash
# Runs every tests/integration/test_*.sh sequentially. Each test gets an
# isolated PG schema and store via lib.sh; cleanup runs even on failure.

set -uo pipefail

TESTS_DIR=$(cd "$(dirname "$0")" && pwd)
LOG_DIR="$TESTS_DIR/.logs"
mkdir -p "$LOG_DIR"
rm -f "$LOG_DIR"/*.log

shopt -s nullglob

pass=0; fail=0
failed=()

for t in "$TESTS_DIR"/test_*.sh; do
  name=$(basename "$t" .sh)
  printf '%-32s ... ' "$name"
  if bash "$t" >"$LOG_DIR/$name.log" 2>&1; then
    echo 'ok'
    pass=$((pass + 1))
  else
    echo 'FAIL'
    fail=$((fail + 1))
    failed+=("$name")
  fi
done

echo
echo "integration: passed $pass, failed $fail"

if (( fail > 0 )); then
  echo
  echo "=================== failure logs ==================="
  for f in "${failed[@]}"; do
    echo
    echo "---- $f ----"
    cat "$LOG_DIR/$f.log"
  done
  exit 1
fi
