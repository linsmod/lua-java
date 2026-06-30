#!/bin/bash
# Run all test scripts through lua_test and report results
set -euo pipefail

LUA_TEST="./build/lua_test"
PASS=0
FAIL=0
FAIL_LIST=""

# All test Java files
TESTS=(
  scripts/tests/01_basic_types.java
  scripts/tests/02_if_else.java
  scripts/tests/03_enum.java
  scripts/tests/04_switch.java
  scripts/tests/05_for_loop.java
  scripts/tests/06_while_loop.java
  scripts/tests/07_dowhile_loop.java
  scripts/tests/08_array.java
  scripts/tests/09_static_method.java
  scripts/tests/10_varargs.java
  scripts/tests/11_constructor.java
  scripts/tests/12_instance_method.java
  scripts/tests/13_method_overload.java
  scripts/tests/14_exception.java
  scripts/tests/15_arraylist.java
  scripts/tests/16_constant.java
  scripts/tests/17_math_operations.java
  scripts/tests/18_import_test.java
  scripts/tests/19_hashmap_sb.java
  scripts/tests/19_minimal.java
  scripts/tests/20_wildcard_import.java
  scripts/tests/21_null_literal.java
  scripts/tests/22_enhanced_for.java
  scripts/tests/23_foreach_syntax.java
  scripts/tests/24_array_index.java
  scripts/test.java
  scripts/test2.java
  scripts/test_instance.java
  scripts/test_min.java
  scripts/test_min2.java
  demo/ComprehensiveDemo.java
  demo/DemoPerson.java
  demo/DemoUtils.java
  scripts/com/example/demo/JavaFeaturesDemo.java
)

run_one() {
  local f="$1"
  local tmp
  tmp=$(mktemp /tmp/lua_test_XXXXXX.log)
  if "$LUA_TEST" "$f" > "$tmp" 2>&1; then
    echo "  PASS  $f"
    PASS=$((PASS + 1))
  else
    echo "  FAIL  $f"
    FAIL=$((FAIL + 1))
    FAIL_LIST="$FAIL_LIST  $f"$'\n'
    # Print truncated error output
    echo "  ----- stderr/output -----"
    grep -i 'error\|fail\|traceback\|assert' "$tmp" | head -5 || cat "$tmp" | tail -10
    echo "  -------------------------"
  fi
  rm -f "$tmp"
}

echo "=== Running $(echo ${#TESTS[@]}) test files ==="
echo ""

for f in "${TESTS[@]}"; do
  if [ -f "$f" ]; then
    run_one "$f"
  else
    echo "  SKIP  $f (not found)"
  fi
done

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
if [ "$FAIL" -gt 0 ]; then
  echo "Failed tests:"
  echo "$FAIL_LIST"
  exit 1
fi
exit 0
