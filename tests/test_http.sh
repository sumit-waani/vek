#!/bin/bash
# Integration test for the HTTP server.
# Starts the server in background, tests endpoints with curl, validates responses.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VEK="$PROJECT_DIR/build/vek"
PORT=3001
PASS=0
FAIL=0

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "=== HTTP Server Integration Tests ==="

# Start the server in background
"$VEK" run "$SCRIPT_DIR/programs/http_hello.ve" &
SERVER_PID=$!

# Wait for server to be ready
for i in $(seq 1 20); do
    if curl -s -o /dev/null "http://localhost:$PORT/" 2>/dev/null; then
        break
    fi
    sleep 0.2
done

# Helper function
check() {
    local name="$1"
    local expected="$2"
    local actual="$3"

    if [ "$actual" = "$expected" ]; then
        echo "  PASS $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL $name"
        echo "    Expected: $expected"
        echo "    Got:      $actual"
        FAIL=$((FAIL + 1))
    fi
}

# Test 1: GET /
response=$(curl -s "http://localhost:$PORT/")
check "GET /" "Hello, World!" "$response"

# Test 2: GET /greet/:name
response=$(curl -s "http://localhost:$PORT/greet/Alice")
check "GET /greet/Alice" "Hello, Alice!" "$response"

# Test 3: GET /greet/:name with different name
response=$(curl -s "http://localhost:$PORT/greet/Bob")
check "GET /greet/Bob" "Hello, Bob!" "$response"

# Test 4: GET /json returns JSON
response=$(curl -s "http://localhost:$PORT/json")
# Check it contains the expected JSON fields (order may vary)
if echo "$response" | grep -q '"message"' && echo "$response" | grep -q '"count"'; then
    echo "  PASS GET /json (contains expected fields)"
    PASS=$((PASS + 1))
else
    echo "  FAIL GET /json"
    echo "    Got: $response"
    FAIL=$((FAIL + 1))
fi

# Test 5: POST /echo
response=$(curl -s -X POST -d "test body" "http://localhost:$PORT/echo")
check "POST /echo" "test body" "$response"

# Test 6: 404 for unknown route
status=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:$PORT/unknown")
check "404 for /unknown" "404" "$status"

# Test 7: Keep-alive (multiple requests on same connection)
response=$(curl -s "http://localhost:$PORT/" "http://localhost:$PORT/greet/KeepAlive")
# curl with multiple URLs on keep-alive concatenates responses
if echo "$response" | grep -q "Hello, World!" && echo "$response" | grep -q "Hello, KeepAlive!"; then
    echo "  PASS Keep-alive (multiple requests)"
    PASS=$((PASS + 1))
else
    echo "  FAIL Keep-alive"
    echo "    Got: $response"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "$PASS passed, $FAIL failed"

# Cleanup
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

[ $FAIL -eq 0 ]
