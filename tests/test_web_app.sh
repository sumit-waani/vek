#!/bin/bash
# Comprehensive web app integration test.
# Tests the full stack: HTTP server, routing, JSON, kv store, path, uuid, logging.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VEK="$PROJECT_DIR/build/vek"
PORT=3002
PASS=0
FAIL=0

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "=== Web App Integration Tests ==="

# Start the server in background
"$VEK" run "$SCRIPT_DIR/programs/web_app_demo.ve" &
SERVER_PID=$!

# Wait for server to be ready
for i in $(seq 1 30); do
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

check_contains() {
    local name="$1"
    local expected="$2"
    local actual="$3"

    if echo "$actual" | grep -q "$expected"; then
        echo "  PASS $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL $name"
        echo "    Expected to contain: $expected"
        echo "    Got: $actual"
        FAIL=$((FAIL + 1))
    fi
}

# Test 1: GET / - HTML response
response=$(curl -s "http://localhost:$PORT/")
check "GET / returns HTML" "<h1>Welcome to Vek Web App</h1>" "$response"

# Test 2: GET /api/status - JSON with expected fields
response=$(curl -s "http://localhost:$PORT/api/status")
check_contains "GET /api/status has status field" '"status":"ok"' "$response"
check_contains "GET /api/status has server_id field" '"server_id":"' "$response"
check_contains "GET /api/status has version field" '"version":"0.1.0"' "$response"

# Test 3: Validate UUID format in status response (36 chars, correct pattern)
uuid=$(echo "$response" | grep -o '"server_id":"[^"]*"' | cut -d'"' -f4)
if echo "$uuid" | grep -qE '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$'; then
    echo "  PASS UUID v4 format is valid ($uuid)"
    PASS=$((PASS + 1))
else
    echo "  FAIL UUID v4 format is invalid ($uuid)"
    FAIL=$((FAIL + 1))
fi

# Test 4: GET /api/echo/:message - path params
response=$(curl -s "http://localhost:$PORT/api/echo/hello-world")
check_contains "GET /api/echo/:message echoes message" '"echo":"hello-world"' "$response"
check_contains "GET /api/echo/:message has path" '"path":"/api/echo/hello-world"' "$response"

# Test 5: POST /api/kv - store a value
response=$(curl -s -X POST -d '{"key":"greeting","value":"hello"}' "http://localhost:$PORT/api/kv")
check_contains "POST /api/kv stores value" '"stored":true' "$response"
check_contains "POST /api/kv returns key" '"key":"greeting"' "$response"

# Test 6: GET /api/kv/:key - retrieve stored value
response=$(curl -s "http://localhost:$PORT/api/kv/greeting")
check_contains "GET /api/kv/:key finds value" '"value":"hello"' "$response"
check_contains "GET /api/kv/:key found is true" '"found":true' "$response"

# Test 7: GET /api/kv/:key - retrieve non-existent key
response=$(curl -s "http://localhost:$PORT/api/kv/nonexistent")
check_contains "GET /api/kv/:key (missing) found is false" '"found":false' "$response"

# Test 8: DELETE /api/kv/:key - delete entry then verify gone
curl -s -X DELETE "http://localhost:$PORT/api/kv/greeting" > /dev/null
response=$(curl -s "http://localhost:$PORT/api/kv/greeting")
check_contains "DELETE then GET /api/kv/:key returns not found" '"found":false' "$response"

# Test 9: POST multiple values and retrieve (state persistence)
curl -s -X POST -d '{"key":"name","value":"vek"}' "http://localhost:$PORT/api/kv" > /dev/null
curl -s -X POST -d '{"key":"lang","value":"c11"}' "http://localhost:$PORT/api/kv" > /dev/null
resp1=$(curl -s "http://localhost:$PORT/api/kv/name")
resp2=$(curl -s "http://localhost:$PORT/api/kv/lang")
check_contains "KV persistence - key 'name'" '"value":"vek"' "$resp1"
check_contains "KV persistence - key 'lang'" '"value":"c11"' "$resp2"

# Test 10: GET /api/path/join - path utility
response=$(curl -s "http://localhost:$PORT/api/path/join")
check_contains "path.join produces correct result" '"joined":"/api/users/42/profile"' "$response"
check_contains "path.basename returns last segment" '"basename":"profile"' "$response"
check_contains "path.dirname returns parent" '"dirname":"/api/users/42"' "$response"

# Test 11: 404 handling
status=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:$PORT/nonexistent")
check "404 for unknown route" "404" "$status"

# Test 12: Another echo to ensure server still works after many requests
response=$(curl -s "http://localhost:$PORT/api/echo/final-test")
check_contains "Server still works after many requests" '"echo":"final-test"' "$response"

echo ""
echo "$PASS passed, $FAIL failed"

# Cleanup
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

[ $FAIL -eq 0 ]
