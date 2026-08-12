# vek - Language Specification (v1)

> Condensed language reference for the vek programming language.

---

## Syntax Family

Ruby/Elixir inspired. Blocks are delimited with `end` (no significant indentation). Semicolons are optional (newline-terminated statements).

---

## Value Types

Eight kinds, all fitting in a NaN-boxed 64-bit value:

| Type | Example | Notes |
|---|---|---|
| `nil` | `nil` | Single value; falsy |
| `bool` | `true`, `false` | Two values; `false` is falsy |
| `int` | `42`, `-1`, `1_000` | 48-bit signed integer |
| `float` | `3.14`, `1e9` | IEEE 754 double |
| `string` | `"hi"`, `'hi'` | UTF-8, immutable, interned when identifier-like |
| `bytes` | `<<1,2,3>>` | Raw byte buffer |
| `list` | `[1, 2, 3]` | Heterogeneous, ordered, dynamic array |
| `map` | `{a: 1, b: 2}` | Insertion-ordered; string keys only in v1 |

Pointers to heap objects are internal and not directly exposed to user code.

---

## Variables and Constants

```ruby
x = 10                  # reassignable local
name = "vek"            # type inferred
port: int = 3000        # optional type annotation (advisory)
PORT = 80               # UPPERCASE = constant (compile-time enforced, not reassignable)
```

### Scoping Rules

- `app.ve` top-level `fn` definitions are global (visible everywhere).
- Top-level definitions in `pages/*.ve` and `views/*.ve` are file-scoped.
- Block scopes (inside `if`, `while`, `for`, `fn`) are lexical.
- No `module`, `import`, `require`, or `use` keywords.

---

## Control Flow

### if / elsif / else

```ruby
if cond
  ...
elsif other
  ...
else
  ...
end
```

`if` is an expression:
```ruby
label = if active? then "on" else "off" end
```

Inline form:
```ruby
redirect "/login" unless user
halt 403 if banned?
```

### while

```ruby
while cond
  ...
end
```

### until

```ruby
until cond
  ...
end
```

### loop

```ruby
loop do
  break if done?
  next if skip?
  ...
end
```

### for..in

```ruby
for x in list
  ...
end

for k, v in map
  ...
end
```

**Loop variable capture:** `for` creates a fresh binding per iteration. Closures created inside the loop capture the per-iteration value (no classic closure-in-loop bug).

### case..in (Pattern Matching)

```ruby
case status
in 200..299 then :ok
in 300..399 then :redirect
in 400..499 then :client_err
in 500..599 then :server_err
in nil      then :no_response
end
```

`case` is an expression. Supports literal patterns, ranges, `nil`, and variable binding. No destructuring in v1.

### break and next

- `break` exits the innermost loop.
- `next` skips to the next iteration.
- Both work inside `while`, `until`, `loop`, and `for`.

---

## Functions

### Definition

```ruby
fn add(a, b)
  a + b
end

fn add_typed(a: int, b: int) -> int
  a + b
end
```

- Last expression is the implicit return value.
- `return` is allowed for early exit.
- Type annotations are advisory (guide opcode selection, no enforcement in v1).

### Closures

Functions capture variables from their enclosing scope by reference:

```ruby
fn make_counter()
  n = 0
  fn()
    n = n + 1
    n
  end
end

c = make_counter()
c()  # 1
c()  # 2
```

### Tail Call Optimization

Calls in tail position reuse the current call frame (no stack growth for recursive functions in tail position).

---

## Lambdas and Blocks

### Lambda Syntax

```ruby
dbl = ->(x) { x * 2 }
dbl.call(5)              # 10
```

### Block Syntax

```ruby
list.map { |x| x * 2 }

list.each do |x|
  log x
end
```

`{ ... }` and `do ... end` are equivalent. Convention: `{}` for single-line, `do/end` for multi-line.

---

## String Interpolation

```ruby
"hello #{name}, you are #{age} years old"
```

Compile-time desugared to: `concat("hello ", name, ", you are ", age, " years old")`

Both `"double quotes"` and `'single quotes'` support interpolation.

---

## Symbol Literals

```ruby
:foo
:"with-dashes"
:"with spaces"
```

A symbol `:foo` is syntactic sugar for an interned string. There is no separate Symbol type.

- `:foo == "foo"` is `true`
- Comparison is pointer-equality after interning
- Only identifier-like forms are valid (no interpolation in symbols)
- Used for config values: `db :sqlite`, `session :cookie`

---

## Operators

### Arithmetic
```
+  -  *  /  %
```

`int / int` always returns `float` (Python 3 rule). Use `x.div(y)` for integer division.

If either operand is `float`, the result is `float`.

### Comparison
```
==  !=  <  >  <=  >=
```

### Logical
```
&&  ||  !
```

Short-circuit evaluation. Return the deciding value (not a coerced bool):
```ruby
nil   || "default"   # "default"
false || "default"   # "default"
0     || "default"   # 0  (0 is truthy!)
"x"   && 42          # 42
```

### Bitwise (int only)
```
&  |  ^  ~  <<  >>
```

### Range
```
..    # inclusive (1..5 includes 5)
...   # exclusive (1...5 excludes 5)
```

### Other
```
?:    # ternary
.     # member access / method call
?.    # safe navigation (returns nil on nil receiver)
[]    # index access
[]=   # index assignment
=     # assignment
```

No operator overloading. Built-in types own the meaning of all operators.

---

## Truthy and Falsy

**Only `nil` and `false` are falsy. Everything else is truthy.**

```ruby
if 0    then "yes" end   # "yes" (0 is truthy)
if ""   then "yes" end   # "yes" (empty string is truthy)
if []   then "yes" end   # "yes" (empty list is truthy)
if {}   then "yes" end   # "yes" (empty map is truthy)
if nil  then "yes" else "no" end   # "no"
if false then "yes" else "no" end  # "no"
```

`!x` returns `true` if `x` is falsy, `false` otherwise.

---

## Methods on Built-in Types

### String Methods

`length`, `slice(start, end)`, `upper`, `lower`, `trim`, `starts_with(s)`, `ends_with(s)`, `contains(s)`, `split(sep)`, `replace(old, new)`, `to_i`, `to_f`, `to_bytes`, `bytes`, `chars`, `index_of(s)`, `repeat(n)`, `pad_left(n, char)`, `pad_right(n, char)`, `is_empty`

### List Methods

`length`, `is_empty`, `first`, `last`, `push(v)`, `pop`, `shift`, `unshift(v)`, `insert(i, v)`, `remove(i)`, `slice(start, end)`, `map { |x| ... }`, `filter { |x| ... }`, `reduce(init) { |acc, x| ... }`, `find { |x| ... }`, `any? { |x| ... }`, `all? { |x| ... }`, `sort`, `sort_by { |x| ... }`, `reverse`, `join(sep)`, `contains(v)`, `uniq`, `flatten`, `zip(other)`, `each { |x| ... }`, `each_with_index { |x, i| ... }`

### Map Methods

`length`, `is_empty`, `get(key)`, `set(key, val)`, `delete(key)`, `has(key)`, `keys`, `values`, `entries`, `merge(other)`, `each { |k, v| ... }`, `map { |k, v| ... }`, `filter { |k, v| ... }`

### Bytes Methods

Indexable as integers. Used for binary data (file uploads, crypto output).

---

## Error Handling

### Two-Tier Model

**Tier 1: Result values for expected failures**

Functions that can fail predictably return `nil` or a tagged value:
```ruby
row = db.row("select * from users where id = ?", id)
case row
in nil then not_found
in user then render "show.ve", user: user
end
```

Optional type annotation syntax:
```ruby
fn parse_int(s) -> int?    # returns int or nil
```

**Tier 2: raise/rescue for unexpected failures**

```ruby
fn load_config(path)
  data = file.read(path) or raise "config not found: #{path}"
  parse(data)
end

begin
  do_thing()
rescue ParseError as e
  log.error "parse failed", error: e.message
  render_500
end
```

- `raise` panics up the call stack
- `rescue` catches raised errors
- Uncaught `raise` in a handler produces a 500 response
- Uncaught `raise` in a CLI/boot context exits the process

### Unwind Signal (Non-Catchable)

`redirect` and `halt` raise a special `Unwind` signal:

```ruby
redirect "/login"           # 302 redirect, unwinds
redirect "/", status: 301   # 301 redirect, unwinds
halt 403, "forbidden"       # sets status+body, unwinds
```

`Unwind` is:
- NOT caught by `rescue` blocks
- Only caught by the request-handler framework boundary
- Implemented as `longjmp` to the nearest handler frame
- Rolls back any open `db.transaction` silently

This prevents a stray `rescue` from swallowing a `redirect` and continuing to execute protected code.

---

## Control Flow Primitives

### redirect

```ruby
redirect url                  # 302
redirect url, status: 301     # custom status
redirect "/login" unless user # inline form
```

### halt

```ruby
halt 403, "forbidden"
halt :not_found              # shorthand
halt 204                     # no body
```

### or (short-circuit with Unwind)

```ruby
user = current_user(req) or redirect "/login"
data = cache.get(key) or fetch_fresh()
```

---

## Numeric Behavior

- `int` is 48-bit signed (range: approximately -140 trillion to +140 trillion)
- `int / int` returns `float` (use `.div()` for integer division)
- Overflow in v1 is a panic (BigInt is v2)
- `1 + 1.0` returns `float` (any float operand promotes the result)
- Underscore separators: `1_000_000`

---

## Concurrency (User Perspective)

User code is logically single-threaded. Blocking I/O looks synchronous:

```ruby
rows = db.query("select * from posts")  # fiber suspends, resumes with result
resp = http.get("https://api.example.com/data")
```

No `async`, no `await`, no callbacks, no function coloring. The fiber scheduler handles suspension and resumption transparently.

Background work uses the `jobs` package:
```ruby
jobs.enqueue "send_email", to: user.email
```

---

## Reserved Keywords

```
nil true false
if elsif else end then unless
while until loop do
for in
case
fn return
begin rescue raise
break next
and or not
```

---

## Comments

```ruby
# single-line comment (no multi-line comment syntax in v1)
```
