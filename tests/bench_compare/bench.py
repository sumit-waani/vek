"""vek VM benchmark equivalents in Python
Each benchmark runs the same workload as bench_vm.c
"""

import time

def time_ms():
    return time.perf_counter() * 1000.0

def run_bench(name, fn, iterations):
    # warmup
    fn()

    start = time_ms()
    for _ in range(iterations):
        fn()
    elapsed = time_ms() - start
    per_op = (elapsed / iterations) * 1000.0  # microseconds
    return (name, iterations, elapsed, per_op)

results = []

# 1. Arithmetic loop (1M iterations)
def bench_arithmetic():
    i = 0
    while i < 1_000_000:
        i += 1

results.append(run_bench("arithmetic_loop (1M)", bench_arithmetic, 5))

# 2. Function calls (1M calls)
def add(a, b):
    return a + b

def bench_function_calls():
    i = 0
    while i < 1_000_000:
        add(i, 1)
        i += 1

results.append(run_bench("function_calls (1M)", bench_function_calls, 5))

# 3. Closure creation (100K)
def make_adder(n):
    def adder(x):
        return n + x
    return adder

def bench_closures():
    i = 0
    while i < 100_000:
        a = make_adder(i)
        a(1)
        i += 1

results.append(run_bench("closure_creation (100K)", bench_closures, 5))

# 4. String concatenation (500)
def bench_string_concat():
    s = ""
    for i in range(500):
        s = s + "x"

results.append(run_bench("string_concat (500)", bench_string_concat, 5))

# 5. List append (100K)
def bench_list_append():
    lst = []
    for i in range(100_000):
        lst.append(i)

results.append(run_bench("list_append (100K)", bench_list_append, 5))

# 6. Map insert (10K)
def bench_map_insert():
    m = {}
    for i in range(10_000):
        m[f"k{i}"] = i

results.append(run_bench("map_insert (10K)", bench_map_insert, 5))

# 7. GC stress (100K allocations)
def bench_gc_stress():
    for i in range(100_000):
        lst = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
        m = {"a": 1, "b": 2, "c": 3}

results.append(run_bench("gc_stress (100K)", bench_gc_stress, 5))

# Print results
print()
print(f"{'Benchmark':<25s} {'Iters':>10s} {'Total (ms)':>12s} {'Per-op (us)':>12s}")
print(f"{'-------------------------':<25s} {'----------':>10s} {'------------':>12s} {'------------':>12s}")
for name, iters, total, per_op in results:
    print(f"{name:<25s} {iters:>10d} {total:>12.2f} {per_op:>12.2f}")
print()
