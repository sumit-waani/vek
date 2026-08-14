/*
 * Benchmark framework for the vek VM.
 * Compiles and runs vek programs, measuring wall-clock time.
 * Uses clock_gettime(CLOCK_MONOTONIC) for accurate timing.
 *
 * Build: make bench
 */

#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common.h"
#include "vm.h"
#include "gc.h"
#include "memory.h"
#include "object.h"
#include "vek_stdlib.h"

// Benchmark result
typedef struct {
    const char *name;
    int iterations;
    double total_ms;
    double per_op_us;
} BenchResult;

#define MAX_BENCHMARKS 16
static BenchResult results[MAX_BENCHMARKS];
static int result_count = 0;

// Get current time in milliseconds (monotonic)
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

// Run a benchmark: runs the source multiple times within the current VM session
static void run_bench(const char *name, const char *source, int iterations) {
    // Warm up: run once to JIT/cache
    vm_interpret(source);

    double start = now_ms();

    for (int i = 0; i < iterations; i++) {
        InterpretResult res = vm_interpret(source);
        (void)res;
    }

    double end = now_ms();
    double total = end - start;
    double per_op = (total / iterations) * 1000.0; // microseconds per iteration

    results[result_count++] = (BenchResult){
        .name = name,
        .iterations = iterations,
        .total_ms = total,
        .per_op_us = per_op,
    };
}

// Print benchmark results table
static void print_results(void) {
    printf("\n");
    printf("%-25s %10s %12s %12s\n", "Benchmark", "Iters", "Total (ms)", "Per-op (us)");
    printf("%-25s %10s %12s %12s\n", "-------------------------", "----------", "------------", "------------");
    for (int i = 0; i < result_count; i++) {
        printf("%-25s %10d %12.2f %12.2f\n",
               results[i].name,
               results[i].iterations,
               results[i].total_ms,
               results[i].per_op_us);
    }
    printf("\n");
}

// Benchmark sources
static const char *BENCH_ARITHMETIC =
    "i = 0\n"
    "while i < 1000000\n"
    "  i = i + 1\n"
    "end\n"
    "i\n";

static const char *BENCH_FUNCTION_CALLS =
    "fn add(a, b)\n"
    "  a + b\n"
    "end\n"
    "\n"
    "i = 0\n"
    "while i < 1000000\n"
    "  add(i, 1)\n"
    "  i = i + 1\n"
    "end\n";

static const char *BENCH_CLOSURE_CREATION =
    "fn make_adder(n)\n"
    "  fn adder(x)\n"
    "    return n + x\n"
    "  end\n"
    "  return adder\n"
    "end\n"
    "\n"
    "i = 0\n"
    "while i < 100000\n"
    "  a = make_adder(i)\n"
    "  a(1)\n"
    "  i = i + 1\n"
    "end\n";

static const char *BENCH_STRING_CONCAT =
    "s = \"\"\n"
    "i = 0\n"
    "while i < 500\n"
    "  s = s + \"x\"\n"
    "  i = i + 1\n"
    "end\n";

static const char *BENCH_LIST_APPEND =
    "list = []\n"
    "i = 0\n"
    "while i < 100000\n"
    "  list.push(i)\n"
    "  i = i + 1\n"
    "end\n";

static const char *BENCH_MAP_INSERT =
    "m = {}\n"
    "i = 0\n"
    "while i < 10000\n"
    "  key = \"k\" + to_s(i)\n"
    "  m[key] = i\n"
    "  i = i + 1\n"
    "end\n";

static const char *BENCH_GC_STRESS =
    "i = 0\n"
    "while i < 100000\n"
    "  list = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]\n"
    "  m = {a: 1, b: 2, c: 3}\n"
    "  i = i + 1\n"
    "end\n";

int main(void) {
    printf("=== vek VM Benchmarks ===\n\n");

    // Initialize VM subsystems once
    gc_init();
    intern_table_init();
    heap_init();
    vm_init();
    stdlib_init();

    // Run each benchmark
    run_bench("arithmetic_loop (1M)",   BENCH_ARITHMETIC,       5);
    run_bench("function_calls (1M)",    BENCH_FUNCTION_CALLS,   5);
    run_bench("closure_creation (100K)",BENCH_CLOSURE_CREATION, 5);
    run_bench("string_concat (500)",    BENCH_STRING_CONCAT,    5);
    run_bench("list_append (100K)",     BENCH_LIST_APPEND,      5);
    run_bench("map_insert (10K)",       BENCH_MAP_INSERT,       5);
    run_bench("gc_stress (100K)",       BENCH_GC_STRESS,        5);

    print_results();

    // Cleanup
    vm_free();
    intern_table_destroy();
    gc_destroy();
    heap_destroy();

    return 0;
}
