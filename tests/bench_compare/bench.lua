-- vek VM benchmark equivalents in Lua
-- Each benchmark runs the same workload as bench_vm.c

local function time_ms()
    return os.clock() * 1000.0
end

local function run_bench(name, fn, iterations)
    -- warmup
    fn()

    local start = time_ms()
    for i = 1, iterations do
        fn()
    end
    local elapsed = time_ms() - start
    local per_op = (elapsed / iterations) * 1000.0 -- microseconds
    return { name = name, iters = iterations, total_ms = elapsed, per_op_us = per_op }
end

local results = {}

-- 1. Arithmetic loop (1M iterations)
table.insert(results, run_bench("arithmetic_loop (1M)", function()
    local i = 0
    while i < 1000000 do
        i = i + 1
    end
end, 5))

-- 2. Function calls (1M calls)
local function add(a, b)
    return a + b
end
table.insert(results, run_bench("function_calls (1M)", function()
    local i = 0
    while i < 1000000 do
        add(i, 1)
        i = i + 1
    end
end, 5))

-- 3. Closure creation (100K)
local function make_adder(n)
    return function(x)
        return n + x
    end
end
table.insert(results, run_bench("closure_creation (100K)", function()
    local i = 0
    while i < 100000 do
        local a = make_adder(i)
        a(1)
        i = i + 1
    end
end, 5))

-- 4. String concatenation (500)
table.insert(results, run_bench("string_concat (500)", function()
    local s = ""
    for i = 1, 500 do
        s = s .. "x"
    end
end, 5))

-- 5. List append (100K)
table.insert(results, run_bench("list_append (100K)", function()
    local list = {}
    for i = 1, 100000 do
        list[#list + 1] = i
    end
end, 5))

-- 6. Map insert (10K)
table.insert(results, run_bench("map_insert (10K)", function()
    local m = {}
    for i = 1, 10000 do
        m["k" .. i] = i
    end
end, 5))

-- 7. GC stress (100K allocations)
table.insert(results, run_bench("gc_stress (100K)", function()
    for i = 1, 100000 do
        local list = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}
        local m = {a = 1, b = 2, c = 3}
    end
end, 5))

-- Print results
print()
print(string.format("%-25s %10s %12s %12s", "Benchmark", "Iters", "Total (ms)", "Per-op (us)"))
print(string.format("%-25s %10s %12s %12s", "-------------------------", "----------", "------------", "------------"))
for _, r in ipairs(results) do
    print(string.format("%-25s %10d %12.2f %12.2f", r.name, r.iters, r.total_ms, r.per_op_us))
end
print()
