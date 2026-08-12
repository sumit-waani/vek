#!/bin/bash
# Run each .ve file in tests/programs/ and compare output to .expected
# Exit code 0 if all pass, 1 if any fail.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VEK="$PROJECT_DIR/build/vek"
PROGRAMS_DIR="$SCRIPT_DIR/programs"

PASS=0
FAIL=0

for ve_file in "$PROGRAMS_DIR"/*.ve; do
    expected="${ve_file%.ve}.expected"
    test_name="$(basename "$ve_file" .ve)"

    if [ ! -f "$expected" ]; then
        echo "  SKIP $test_name (no .expected file)"
        continue
    fi

    actual=$("$VEK" run "$ve_file" 2>/dev/null)
    expected_content=$(cat "$expected")

    if [ "$actual" = "$expected_content" ]; then
        PASS=$((PASS + 1))
        echo "  PASS $test_name"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL $test_name"
        echo "    Expected:"
        echo "$expected_content" | head -5 | sed 's/^/      /'
        echo "    Got:"
        echo "$actual" | head -5 | sed 's/^/      /'
    fi
done

echo ""
echo "$PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
