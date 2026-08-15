#!/usr/bin/env bash
# Comparative benchmark: vek vs Lua vs LuaJIT vs CPython
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
VEK_BENCH="$REPO_ROOT/build/bench_vm"

for cmd in lua luajit python3 "$VEK_BENCH"; do
    if ! command -v "$cmd" &>/dev/null && [ ! -x "$cmd" ]; then
        echo "Error: $cmd not found."
        exit 1
    fi
done

echo "=== vek Language Benchmark Comparison ==="
echo ""
echo "Identical workloads: vek | Lua 5.4 | LuaJIT | CPython 3.12"
echo "Each: 1 warmup + 5 measured iterations"
echo ""

run_and_save() {
    local label="$1" outfile="$2" cmd="$2"
    echo "[$label]"
    "$@" 2>&1 | tee "$outfile"
    echo ""
}

echo "[1/4] vek..."
"$VEK_BENCH" > /tmp/bench_vek.txt 2>&1 && cat /tmp/bench_vek.txt
echo ""

echo "[2/4] Lua 5.4..."
lua "$SCRIPT_DIR/bench.lua" > /tmp/bench_lua.txt 2>&1 && cat /tmp/bench_lua.txt
echo ""

echo "[3/4] LuaJIT..."
luajit "$SCRIPT_DIR/bench.lua" > /tmp/bench_luajit.txt 2>&1 && cat /tmp/bench_luajit.txt
echo ""

echo "[4/4] CPython 3.12..."
python3 "$SCRIPT_DIR/bench.py" > /tmp/bench_python.txt 2>&1 && cat /tmp/bench_python.txt
echo ""

# Use python for clean table formatting
python3 << 'PYEOF'
import sys

def extract_per_op(filepath):
    """Extract per-op microseconds (last column) from benchmark output."""
    vals = []
    with open(filepath) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 4:
                try:
                    # Last column is always per-op (us)
                    v = float(parts[-1])
                    # Sanity: per-op values should be > 0 and we skip header/separator lines
                    if v > 0 and not parts[0].startswith('-') and parts[0] != 'Benchmark':
                        vals.append(v)
                except ValueError:
                    pass
    return vals

vek = extract_per_op("/tmp/bench_vek.txt")
lua = extract_per_op("/tmp/bench_lua.txt")
jit = extract_per_op("/tmp/bench_luajit.txt")
py  = extract_per_op("/tmp/bench_python.txt")

names = [
    "arithmetic_loop (1M)",
    "function_calls (1M)",
    "closure_creation (100K)",
    "string_concat (500)",
    "list_append (100K)",
    "map_insert (10K)",
    "gc_stress (100K)",
]

def fmt(us):
    if us >= 1_000_000: return f"{us/1_000_000:.1f}s"
    if us >= 1_000:     return f"{us/1_000:.1f}ms"
    return f"{us:.0f}us"

def ratio_str(vek_us, ref_us):
    if ref_us <= 0: return "-"
    r = vek_us / ref_us
    if r <= 1.0:
        return f"{1/r:.0f}x FASTER"
    else:
        return f"{r:.0f}x slower"

print()
print(f"{'Benchmark':<27} {'vek':>10} {'Lua 5.4':>10} {'LuaJIT':>10} {'CPython':>10}  {'vek vs LuaJIT':>15}")
print("-" * 92)

count = min(len(names), len(vek), len(lua), len(jit), len(py))
for i in range(count):
    print(f"{names[i]:<27} {fmt(vek[i]):>10} {fmt(lua[i]):>10} {fmt(jit[i]):>10} {fmt(py[i]):>10}  {ratio_str(vek[i], jit[i]):>15}")

print()

# Summary
vek_sum = sum(vek[:count])
lua_sum = sum(lua[:count])
jit_sum = sum(jit[:count])
py_sum  = sum(py[:count])

print("=== SUMMARY ===")
print(f"  vek total:     {fmt(vek_sum)}")
print(f"  Lua 5.4 total: {fmt(lua_sum)}  ({vek_sum/lua_sum:.1f}x vs vek)")
print(f"  LuaJIT total:  {fmt(jit_sum)}  ({vek_sum/jit_sum:.1f}x vs vek)")
print(f"  CPython total: {fmt(py_sum)}  ({vek_sum/py_sum:.1f}x vs vek)")
print()
print("Closest peer: Lua 5.4 (interpreted, register-based VM like vek)")
print("LuaJIT: trace-compiled JIT -- different class entirely")
PYEOF
