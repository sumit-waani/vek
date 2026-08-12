# vek — Design Document

> A web-first language, runtime, and one-binary deployment story for people who want to ship small apps without negotiating with an ecosystem.

**Status:** Revision 2 (post-review patch — see §30 for resolved questions and the threads around I/O model, language gaps, and web-first specifics)
**Audience:** The author (and anyone who later picks this up)
**Goal of this document:** Lock in every decision that would otherwise be relitigated three times during implementation. If it's not in here, it's not decided.

---

## Table of Contents

1. [Why vek Exists](#1-why-vek-exists)
2. [Non-Goals](#2-non-goals)
3. [High-Level Architecture](#3-high-level-architecture)
4. [Language Design](#4-language-design)
5. [Project Layout](#5-project-layout)
6. [The `app.ve` File](#6-the-appve-file)
7. [Pages & Routing](#7-pages--routing)
8. [Views, Layouts, Partials](#8-views-layouts-partials)
9. [The Built-In 25-30 Stdlib Packages](#9-the-built-in-25-30-stdlib-packages)
10. [Persistence: SQLite, Postgres, KV](#10-persistence-sqlite-postgres-kv)
11. [Compiler Pipeline & Bytecode Format](#11-compiler-pipeline--bytecode-format)
12. [Virtual Machine](#12-virtual-machine)
13. [Garbage Collector](#13-garbage-collector)
14. [String, List, Map Internals](#14-string-list-map-internals)
15. [Concurrency & I/O](#15-concurrency--io)
16. [Error Handling](#16-error-handling)
17. [HTTP Server, Routing, Sessions, CSRF](#17-http-server-routing-sessions-csrf)
18. [Jobs & Mail](#18-jobs--mail)
19. [Logging & Observability](#19-logging--observability)
20. [CLI (`vek`)](#20-cli-vek)
21. [Dashboard (`vekd`)](#21-dashboard-vekd)
22. [Deploy Flow End-to-End](#22-deploy-flow-end-to-end)
23. [Process Supervision & cgroups](#23-process-supervision--cgroups)
24. [Cloudflare Integration](#24-cloudflare-integration)
25. [Reverse Proxy & Routing](#25-reverse-proxy--routing)
26. [Security Model](#26-security-model)
27. [Build Artifact Format (`.vebc`)](#27-build-artifact-format-vebc)
28. [Performance Budget](#28-performance-budget)
29. [Roadmap (v1 / v2 / v3)](#29-roadmap-v1--v2--v3)
30. [Open Questions](#30-open-questions)
31. [Appendix A — Why These Choices](#31-appendix-a--why-these-choices)
32. [Appendix B — Quick Reference: One App](#32-appendix-b--quick-reference-one-app)

---

## 1. Why vek Exists

The web platform forces every developer to do the same five things to ship a small app:

1. Pick a runtime (Node, Deno, Bun, Workers).
2. Pick a framework (Next, Remix, SvelteKit, Hono, …).
3. Pick a package manager and wrestle with `node_modules` (often 200 MB+).
4. Pick a deployment target (Vercel, Cloudflare, Fly, your own VPS).
5. Find a database story that doesn't break across all of the above.

Each choice interacts with the others. Vercel's runtime ≠ Cloudflare's. A package that works in Node might not work in Workers. The user has to know all of this before they can write `hello world`.

PHP solved this once with the LAMP stack — one stack, one deploy, one model. It's still the cheapest path to a live web app. But its ecosystem stagnated and the language ergonomics drove people away.

**vek is the bet that we can build a PHP-class deploy story (single binary, single VPS, one command) with modern language ergonomics and a web-flavored stdlib.** No node_modules. No `import`. No runtime selection matrix. One CLI, one dashboard, one `.vebc` artifact.

Target user: a systems-leaning developer who wants to ship a small web app (blog, side project, internal tool, SaaS MVP) on a $5 VPS and never think about a build pipeline again.

---

## 2. Non-Goals

Explicit. Don't drift into these.

- **No distributed runtime.** v1 is single-process, single-VM, single-VPS. Multi-node is a v3 problem.
- **No package ecosystem.** No npm, no crates.io, no central registry. The 25-30 built-in packages are the universe. If you need something else, vendor it or write it in `app.ve`.
- **No IDE integration.** A formatter and a syntax file for one editor (vim) is the bar. LSP is a v2 stretch.
- **No FFI.** No calling C, no calling JS. v1 is hermetic.
- **No mobile, no desktop, no WASM target.** Server-rendered HTML is the focus. WASM-as-target is interesting but out of scope.
- **No multi-tenant hosting.** vekd is single-user (one sysadmin) on a single VPS. Multi-user is a v2 problem.
- **No dynamic code loading.** No `eval`, no hot-patching compiled code. The dev server can re-eval source files on save, but the deployed binary is immutable.
- **No language design maximalism.** No traits, no GADTs, no higher-kinded types, no metaprogramming. If Elixir 1.0 wouldn't have it, v1 probably doesn't either.

---

## 3. High-Level Architecture

```
   ┌──────────────────────────────────────────────────────────────────┐
   │  VPS (one machine, one user, one systemd service)                │
   │                                                                  │
   │  ┌─────────────┐  host-header   ┌─────────────────────────────┐  │
   │  │   vekd      │◀──────────────▶│  vek app 1 (port 10001)     │  │
   │  │  :80/:443   │  routing       │  vek app 2 (port 10002)     │  │
   │  │  :8080 UI   │                │  vek app 3 (port 10003)     │  │
   │  │  supervisor │                │  ...                        │  │
   │  │  cgroup mgr │                │  each in its own cgroup     │  │
   │  │  CF client  │                │  each runs as its own user  │  │
   │  └─────┬───────┘                └─────────────────────────────┘  │
   │        │                                                          │
   │        │ cloudflared (optional tunnel)                            │
   └────────┼─────────────────────────────────────────────────────────┘
            │
            ▼
       Cloudflare (DNS, cache, optional TLS, DDoS)
            │
            ▼
         Internet
```

```
   ┌────────────────────┐         ┌────────────────────┐
   │  Developer machine │  git    │  GitHub            │
   │  vek new myapp     │────────▶│  me/myapp          │
   │  vek dev           │         └─────────┬──────────┘
   │  vek deploy URL    │                   │ PAT
   └─────────┬──────────┘                   │
             │                              ▼
             │ push          ┌─────────────────────────────┐
             ▼               │  vekd pulls + builds         │
   ───────────────────────   │  runs cgexec vek run         │
                             │  on internal port            │
                             └─────────────────────────────┘
```

Three artifacts:

1. **`vek`** — the CLI + runtime. One binary. Compiles, runs, formats, REPL.
2. **`vekd`** — the dashboard / supervisor. One binary. Runs as a system service.
3. **`.vebc`** — the build artifact. Bytecode + assets + metadata. What `vekd` actually executes.

---

## 4. Language Design

### 4.1 Syntax Family

Ruby / Elixir inspired. Significant indentation is **not** used — we keep `end` to close blocks because:
- editors without indent-aware tooling render it cleanly
- the existing parser tooling is happier
- it makes the boundary between blocks and method calls obvious

### 4.2 Values

Eight kinds. All fit in a NaN-boxed 64-bit value (see §12):

| Tag             | Example          | Notes                           |
|-----------------|------------------|---------------------------------|
| `nil`           | `nil`            | one value                       |
| `bool`          | `true`, `false`  | two values                      |
| `int`           | `42`, `-1`       | 48-bit signed (see §12)         |
| `float`         | `3.14`, `1e9`    | IEEE 754 double                 |
| `string`        | `"hi"`, `'hi'`   | UTF-8, immutable, interned when identifier-like |
| `bytes`         | `<<1,2,3>>`      | raw byte buffer (for crypto, uploads) |
| `list`          | `[1, 2, 3]`      | heterogeneous, ordered          |
| `map`           | `{a: 1, b: 2}`   | insertion-ordered; string keys only in v1 |
| `pointer`       | (internal)       | heap object                     |

That's it. No tuples vs lists, no symbol type, no char. Strings serve as symbols. Integers are int until they aren't, then we panic in v1 (BigInt is a v2 feature; for web apps 48 bits is ~281 trillion, enough).

### 4.3 Variables

```ruby
x = 10                  # reassignable
name = "vek"            # inferred
port: int = 3000        # annotated
PORT = 80               # UPPERCASE → constant (compile-time enforced)
```

### 4.4 Control Flow

```ruby
if cond
  ...
elsif other
  ...
else
  ...
end

while cond
  ...
end

until cond
  ...
end

loop do
  break if cond
  next if skip
  ...
end

for x in list
  ...
end

for k, v in map
  ...
end

case shape
in :circle
  ...
in :square
  ...
in other
  ...
end
```

`if` and `case` are expressions:

```ruby
label = if active? then "on" else "off" end
```

### 4.5 Functions

```ruby
fn add(a, b)
  a + b
end

fn add_typed(a: int, b: int) -> int
  a + b
end

# Last expression is the return value (no `return` needed).
# `return` is allowed for early exit.
```

Closures capture by reference for variables in the enclosing scope.

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

Tail-call optimization: yes, for `fn` calls in tail position.

### 4.6 Lambdas / Blocks

```ruby
dbl = ->(x) { x * 2 }
dbl.call(5)              # 10

list.map { |x| x * 2 }
list.each do |x|
  log x
end
```

`{ ... }` and `do ... end` are equivalent. Use `do/end` for multi-line, `{}` for inline.

### 4.7 String Interpolation

```ruby
"hello #{name}, you are #{age} years old"
```

Compile-time desugared to `concat("hello ", name, ", you are ", age, " years old")`.

### 4.8 Operators

```
+ - * / %          arithmetic
== != < > <= >=    comparison
&& || !            logical
& | ^ ~ << >>      bitwise (int only)
.. ...             range (inclusive, exclusive)
?:                 ternary
.                  member access / method call
?.                 safe nav  (returns nil on nil)
[]                 index
[]=                index assign
=                  assignment
```

No operator overloading. Built-in types own the meaning of `+` etc.

### 4.9 Method Calls on Built-ins

`"hello".upper`, `[1,2,3].map { ... }`, `m.size`. The set of methods on each built-in type is fixed and listed in §14. User code cannot add methods to built-ins in v1.

### 4.10 Pattern Matching

Minimal in v1. Just `case` / `in` with literal patterns and variable binding.

```ruby
case status
in 200..299 then :ok
in 300..399 then :redirect
in 400..499 then :client_err
in 500..599 then :server_err
in nil      then :no_response
end
```

No destructuring of maps in v1. No struct patterns. That is a v2 language feature.

### 4.11 Error Handling — Two Tiers

**Tier 1: Result-like values for expected failures.**

```ruby
row = db.row("select * from users where id = ?", id)
case row
in nil then not_found
in user then render "show.ve", user: user
end
```

For functions that can fail predictably, the convention is to return `nil` or a tagged map:

```ruby
fn parse_int(s) -> int?
  # returns int or nil
end
```

`int?` is syntactic sugar for `int | nil` (documented but not enforced at the type level in v1 — annotations are advisory).

**Tier 2: `raise` / `rescue` for unexpected failures.**

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

`raise` panics up the call stack. `rescue` catches. Uncaught `raise` → 500 response in a request handler, process exit in CLI.

### 4.12 Modules / Namespaces

The word "module" in vek refers to **namespacing stdlib packages**, not a code organization unit. There is no `module`, no `import`, no `require`, no `include`, no `use`.

- All stdlib packages are always available.
- All definitions in `app.ve` are **global** (visible to every file).
- Top-level `fn` definitions in `pages/*.ve` and `views/*.ve` are **file-scoped** — they are not visible to other files. Use `app.ve` for any helper shared across files.
- Namespacing happens through naming: `db.query`, `mail.send`, `kv.get`. The dot in `db.query` is just a constant lookup — `db` is a top-level value that holds a "package object" with functions inside.

This is deliberate. v1 has no module system, and an unrestricted global namespace across all `pages/*.ve` files would create silent, load-order-dependent name collisions the moment a project has two helpers with the same name. The rule is: **`app.ve` for shared code, the page/view file for everything else.**

If `pages/posts/index.ve` defines `fn current_post`, it is private to that file. If you need it elsewhere, move it to `app.ve` or pass it explicitly. v2 will add proper `lib/` directories with explicit `use` declarations.

If you need to organize a large file, use `# --- section ---` comments. We're not pretending this scales to a 100k-line codebase — that's a v3 problem with proper modules.

### 4.13 Symbol Literals

A symbol literal `:foo` is **syntactic sugar for an interned string**. There is no separate `Symbol` type. The parser turns `:foo` into a string constant; the runtime interns it on first use (see §14.1).

- `:foo == "foo"` is `true`.
- Both are interned, so `s == :foo` is pointer-equality after interning.
- Symbols are useful for map keys, enum-like values, and the first arg to stdlib config calls (e.g. `db :sqlite, path: ...`).
- Only identifier-like forms are allowed: `:foo`, `:"with spaces"`, `:"with-dashes"`. No `:"interpolated #{x}"` — use a string for that.

The token `:foo` exists in this grammar explicitly because almost every config-style call (`db :sqlite, session :cookie, jobs queue: :default`) reads more naturally than the string equivalent. Under the hood it's a string.

### 4.14 Truthy and Falsy

**Only `nil` and `false` are falsy. Everything else is truthy**, including `0`, `0.0`, `""`, `[]`, `{}`.

```ruby
if 0    then log "yes" end   # logs "yes"
if ""   then log "yes" end   # logs "yes"
if []   then log "yes" end   # logs "yes"
if {}   then log "yes" end   # logs "yes"
if nil  then log "yes" else log "no" end   # logs "no"
if false then log "yes" else log "no" end # logs "no"
```

This is Pythonic and avoids JS-style coercion hell. **`if 0`** is not a typo-trap here; it's deliberate.

`||` and `&&` short-circuit and return the deciding value (not a coerced bool):

```ruby
nil   || "default"   # "default"
false || "default"   # "default"
0     || "default"   # 0
""    || "default"   # ""
"x"   || "default"   # "x"

"x"   && 42          # 42
nil   && side_effect # nil (and side_effect is NOT called)
```

The ternary `?:` follows the same rules.

`!x` returns `true` if `x` is falsy, `false` otherwise.

This rule is in this section explicitly so the patterns `u = current_user(req) or redirect "/login"` and `kv.get("k") or default_value` have a single, non-controversial meaning: a missing or explicitly-false value triggers the fallback; any other value is kept as-is.

### 4.15 Control-Flow Primitives: `redirect`, `halt`, and the `Unwind` Signal

Three built-ins change control flow without ever returning a value:

- `redirect url, status: 302` — sets the `Location` header, marks the response as the given status, and **unwinds**.
- `redirect url, status: 301` — same, permanent.
- `halt status, body` — sets status and body, and **unwinds**. Use for early-exit-anywhere: `halt 403, "go away"`, `halt :not_found`.

`redirect` and `halt` are not regular function calls. They raise a special **non-catchable** signal we call `Unwind`. **`Unwind` is a separate control-flow type from `raise`.** It is caught only by the request-handler framework (the boundary of `get "/" do ... end`). It is **never** caught by `rescue`.

This is a load-bearing decision. If `Unwind` were catchable, every `begin ... rescue` block would be a potential security hole: a stray `rescue` could swallow a `redirect`, and the protected-page code after it would still run. By making `Unwind` non-catchable, we eliminate that class of bug at the language level.

```ruby
fn require_login(req)
  u = current_user(req)
  redirect "/login" unless u   # if u is nil/false, this Unwinds
  u                            # otherwise we get here
end

get "/admin" do
  begin
    require_login(req)         # may Unwind
    render_admin               # only runs if require_login returned
  rescue SomeError             # does NOT catch Unwind
    log "unreachable"
  end
end
```

**Semantics in expressions:**

```ruby
x = if cond then redirect "/foo" else 42 end
```

If `cond` is truthy, the `if` expression Unwinds and the `else` branch is irrelevant. Prefer the `unless` form for clarity:

```ruby
redirect "/foo" if cond
halt 401, "go away" unless authenticated
```

**Implementation:** `Unwind` is a `longjmp` to the nearest request handler frame (or to the current fiber's saved state, see §15). It does not allocate, does not walk the stack to find a handler, and bypasses `rescue` blocks entirely. Fast, and impossible to misuse.

**`Unwind` inside a `db.transaction do ... end`:** if the block Unwinds, the transaction is **rolled back silently** and the request ends. There is no commit, no error. This is the right behavior; see Q13 in §30.

### 4.16 Loop Variable Capture

Loop variables in `for` are **rebound per iteration**. Each iteration gets a fresh binding; closures created inside the loop capture that fresh binding, not the loop variable's outer slot.

```ruby
handlers = []
for i in 0..5
  handlers.push(fn() { i })
end
# handlers[0]() == 0
# handlers[3]() == 3
# handlers[5]() == 5
```

This matches Python (3+), Ruby, Elixir, and modern JS (`for` with `let`/`const`). It does **not** match pre-1.22 Go or pre-ES6 JS with `var`.

**Implementation:** the compiler desugars `for x in iter; body; end` to roughly:

```ruby
iter.each do |x|     # internal; the .each method on the iterable
  body
end
```

Each call to the block creates a new local scope. Closures inside the body capture the block's parameter `x`, which is a fresh local each iteration.

`while`, `until`, and `loop do ... end` do **not** introduce a new scope per iteration — they have no loop variable. They are not a footgun.

### 4.17 Concurrency in Code

In v1, app code is logically single-threaded. The model is **fibers + a worker thread pool** — see §15 for the full design. From the user's point of view, blocking I/O looks synchronous: `db.query(...)` returns rows directly, no `await`, no callbacks. Under the hood the fiber suspends and the I/O runs on a worker thread.

Background work happens in **separate processes** (see §18 Jobs). v2 may add real OS-thread parallelism via green threads; v1 does not.

---

## 5. Project Layout

```
myapp/
├── app.ve                  # entry point + global config + helpers
├── pages/                  # file-based routes (one handler per file)
│   ├── index.ve            # → GET  /
│   ├── about.ve            # → GET  /about
│   ├── posts/
│   │   ├── index.ve        # → GET  /posts, POST /posts
│   │   ├── new.ve          # → GET  /posts/new
│   │   └── [id].ve         # → GET  /posts/:id
│   ├── login.ve            # → GET  /login
│   └── api/
│       └── health.ve       # → GET  /api/health
├── views/                  # reusable view fragments (optional)
│   ├── layouts/
│   │   └── main.ve
│   ├── _post_card.ve       # partial
│   └── _flash.ve
├── public/                 # static assets, served as-is
│   ├── style.css
│   └── app.js
├── migrations/             # SQL files, applied in lexical order
│   ├── 001_users.sql
│   └── 002_posts.sql
├── data/                   # SQLite file lives here
│   └── app.db
├── log/                    # local dev logs
├── vek.toml                # optional project metadata
└── .env                    # local dev env vars (gitignored)
```

Conventions:
- `pages/` is required. vek scans it recursively.
- `views/` is optional. Anything not in `pages/` is not routed.
- File names use kebab-case; route segments inherit the file name.
- Square brackets denote dynamic segments: `[id].ve` → `:id`.
- Underscore-prefixed view files are partials, not standalone.

---

## 6. The `app.ve` File

`app.ve` is the entry point and the only file that is **implicitly available everywhere**. It is loaded once at boot, and its top-level definitions are merged into the global namespace that every page and every other file sees.

You never `require` it. It is just there.

```ruby
# app.ve
# This file is auto-included in every page. Do not import it.

app name: "myblog", port: env("PORT", default: 3000)

# --- database ---
db :sqlite, path: "data/app.db"
# or:  db :postgres, url: env("DATABASE_URL")

# --- sessions ---
session :cookie,
  secret:    env("SESSION_SECRET"),
  name:      "sid",
  max_age:   7 * 24 * 3600,
  secure:    env("APP_ENV") == "production",
  same_site: "lax"

# --- kv / cache ---
kv  max_entries: 50_000
cache max_entries: 10_000, default_ttl: 300

# --- jobs ---
jobs queue: "default", workers: 2, store: :sqlite

# --- mail ---
mail smtp_host: env("SMTP_HOST"),
     smtp_port: env("SMTP_PORT", default: 587),
     username:  env("SMTP_USER"),
     password:  env("SMTP_PASS"),
     from:      "noreply@myblog.com"

# --- storage ---
storage driver: :s3,
        endpoint:  env("S3_ENDPOINT"),
        region:    env("S3_REGION",  default: "auto"),
        bucket:    env("S3_BUCKET"),
        access:    env("S3_ACCESS_KEY"),
        secret:    env("S3_SECRET_KEY")

# --- logging ---
log level: env("LOG_LEVEL", default: "info"), json: env("APP_ENV") == "production"

# --- shared helpers (visible to every page) ---

fn current_user(req)
  uid = req.session["user_id"]
  return nil unless uid
  db.row("select * from users where id = ?", uid)
end

fn require_login(req)
  u = current_user(req)
  redirect "/login" unless u
  u
end

fn flash_error(msg)
  session.flash[:error] = msg
end

# --- boot hook ---
on boot do
  log.info "app booted", name: app.name, port: app.port
end

# --- global before-handler ---
before do |req|
  log.info "request",
    method: req.method, path: req.path, ip: req.ip
end
```

### Why `app.ve` is special

- It is the only file that runs at boot (other files only run when a route is hit).
- Top-level `db`, `session`, `kv`, `mail`, `storage` calls are **configuration** — they don't return a value you assign; they register a driver. Configuration is idempotent: setting `db` twice is a warning, not a fatal error.
- Top-level `fn` definitions are global.
- `on boot`, `on shutdown`, `before`, `after` are registered hooks. Multiple are allowed; order is declaration order.
- `env("KEY")` is the only way to read env vars in v1. Direct `ENV["KEY"]` is not a thing.

---

## 7. Pages & Routing

### 7.1 File-Based Routing

Every file in `pages/` becomes a route. The route path is the file path, minus the `.ve` extension, with `[param]` placeholders.

| File                       | Route              |
|----------------------------|--------------------|
| `pages/index.ve`           | `GET /`            |
| `pages/about.ve`           | `GET /about`       |
| `pages/posts/index.ve`     | `GET /posts`       |
| `pages/posts/new.ve`       | `GET /posts/new`   |
| `pages/posts/[id].ve`      | `GET /posts/:id`   |
| `pages/api/health.ve`      | `GET /api/health`  |
| `pages/api/posts/[id].ve`  | `GET /api/posts/:id` |

### 7.2 HTTP Methods

Each page file declares which methods it handles. Multiple methods per file is allowed.

```ruby
# pages/posts/index.ve
get "/" do
  posts = db.query("select * from posts order by created_at desc")
  render_inline do
    h1 "Posts"
    ul
      for post in posts
        li
          a(href: "/posts/#{post.id}") { post.title }
      end
    a(href: "/posts/new") { "New post" }
  end
end

post "/" do |req|
  require_login(req)
  form.validate(req.body, do
    field "title", required: true, min: 1, max: 200
    field "body",  required: true
  end)
  if form.valid?
    id = db.exec(
      "insert into posts (user_id, title, body) values (?, ?, ?)",
      current_user(req).id, form["title"], form["body"]
    )
    redirect "/posts/#{id}"
  else
    render_inline do
      h1 "New post"
      ul.errors
        for err in form.errors
          li #{err.field}: #{err.message}
        end
      end
    end
  end
end
```

Available method bindings: `get`, `post`, `put`, `patch`, `delete`. Each takes a path-or-block. If a file has only `get` blocks and a `POST` comes in, the response is `405 Method Not Allowed`.

### 7.3 The `req` Object

Inside any handler, `req` is implicit (you can also take it as a parameter for clarity).

| Field             | Type     | Description |
|-------------------|----------|-------------|
| `req.method`      | string   | `"GET"`, `"POST"`, ... |
| `req.path`        | string   | `/posts/42` |
| `req.query`       | map      | parsed querystring; `req.query["q"]` |
| `req.body`        | string/bytes | raw body (for JSON) or form-encoded |
| `req.params`      | map      | dynamic path segments: `req.params["id"]` |
| `req.headers`     | map      | `req.headers["user-agent"]` |
| `req.cookies`     | map      | parsed cookies |
| `req.session`     | map      | current session, lazily loaded |
| `req.ip`          | string   | respecting `X-Forwarded-For` if behind vekd/CF |
| `req.form`        | map      | parsed form body (when Content-Type is `application/x-www-form-urlencoded` or `multipart/form-data`) |
| `req.files`       | list     | uploaded files (only for `multipart/form-data`); see §7.7 |
| `req.csrf_token`  | string   | the per-request CSRF token to put in forms |

### 7.4 Responses

```ruby
render "view.ve", user: user                    # 200, text/html
render_inline do ... end                        # 200, text/html, no template file
render "view.ve", user: user, status: 201       # any status
redirect "/login"                              # 302
redirect "/", status: 301
json user: user                                # 200, application/json
json {error: "not found"}, status: 404
text "hello"                                    # 200, text/plain
file "public/big.pdf"                           # 200, file served
empty 204
```

### 7.5 Dynamic Segments and Constraints

```
pages/posts/[id].ve           → /posts/:id          (any non-slash)
pages/posts/[id(int)].ve      → /posts/:id          (only digits)
pages/posts/[slug].ve         → /posts/:slug        (any non-slash)
pages/files/[...path].ve      → /files/*path        (catch-all)
```

Constraints are in parens, applied to the param. Built-in: `int`, `uuid`, `slug`. Custom constraints are a v2 feature.

### 7.6 Middleware-style Hooks

In `app.ve`:

```ruby
before do |req|
  # runs before every handler
end

after do |req, res|
  # runs after every handler
end
```

`before` can short-circuit by calling `halt status, body`. For per-route middleware in v1, just call a function in the handler. v2 may add a `wrap` block.

### 7.7 File Uploads

For `multipart/form-data` requests, `req.files` is a list of uploaded files. Each file:

| Field         | Type   | Description |
|---------------|--------|-------------|
| `name`        | string | form field name (the `<input name="...">`) |
| `filename`    | string | client-provided filename (untrusted; never use as a path) |
| `content_type`| string | client-provided MIME type (untrusted) |
| `size`        | int    | bytes |
| `data`        | bytes  | file content (in-memory) |

**Size limits** are configured in `app.ve`:

```ruby
upload max_request_size: 50 * 1024 * 1024  # 50 MB total request
upload max_file_size:   20 * 1024 * 1024  # 20 MB per file
upload max_files:       10                # at most 10 files per request
```

If a limit is exceeded, the handler is not called; the response is `413 Payload Too Large`.

**Streaming large uploads:** for files that may exceed the in-memory threshold (default 4 MB), use `req.files_iter` (a generator) to avoid loading everything at once:

```ruby
post "/upload" do
  for f in req.files_iter
    storage.put "uploads/#{uuid.v4}#{path.ext(f.filename)}", f.data,
      content_type: f.content_type
  end
  text "ok"
end
```

Internally, files smaller than the in-memory threshold (default 4 MB) live in RAM; larger ones are written to a temp file under `/tmp/vek-uploads/` and `f.data` is a lazy `Bytes` view that reads from it. Temp files are deleted when the request ends (success, error, or `Unwind`).

**Filename handling:** `f.filename` is the raw string from the client. Never use it as a filesystem path or in a `storage.put` key without sanitization — use `path.safe_filename(f.filename)` (strips `..`, `/`, leading dots, control chars) and `uuid.v4` for the actual key.

**CSRF:** file uploads go through the same CSRF check as forms. The `form` helper auto-includes the token. A raw `multipart/form-data` POST without a valid `_csrf` field is rejected with 403. (If you want to accept uploads from a third party that can't carry a CSRF token, use the `webhook` package or a per-route `skip_csrf` — see §17.4.)

---

## 8. Views, Layouts, Partials

The view DSL is just vek code with helper functions for HTML. No separate template language.

```ruby
# views/layouts/main.ve
fn call(content)
  doctype(:html5)
  html(lang: "en") do
    head do
      title @title
      link(rel: "stylesheet", href: "/style.css")
      meta(charset: "utf-8")
    end
    body do
      header do
        a(href: "/") { "Home" }
        span " | "
        a(href: "/about") { "About" }
      end
      main do
        content()        # this is the inner view
      end
      footer do
        small "Powered by vek"
      end
    end
  end
end
```

```ruby
# views/_post_card.ve
fn call(post)
  article.post_card do
    h2 a(href: "/posts/#{post.id}") { post.title }
    p post.summary
    small post.created_at
  end
end
```

A view file is a module that exposes a single `call(...)` function. Render it like:

```ruby
render "home.ve", title: "Home", posts: posts
```

Or inline in a page:

```ruby
get "/" do
  render_inline do
    layout "main.ve" do
      h1 "Home"
      partial "_post_card.ve", post: posts[0]
    end
  end
end
```

`render_inline` skips the file and uses the block directly. `layout` wraps content in a layout file. `partial` includes a reusable chunk.

### 8.1 Auto-Escaping

All string interpolations and dynamic content inside HTML helpers is HTML-escaped. The `raw "..."` helper emits unescaped HTML (use with care). The `sanitize.html(s)` helper filters against an allowlist.

### 8.2 Why No Separate Template Language

- One language, one parser, one debugger.
- The page file and the view file are the same kind of file; you can copy-paste between them.
- If the page is small, skip the file and use `render_inline`. If it's big, pull it out — no syntax shift.

The trade-off: people who love Slim/Haml/Pug won't get their DSL. That's fine, v1.

### 8.3 Render Buffer (Internal, Not User-Facing)

User code never sees the buffer; this is a runtime implementation detail, called out here because it directly affects the perf budget in §28.

`String` in vek is immutable. Naive `concat` in a 200-element view would be O(n²) — the previous draft of §14.1 admitted this, and it would blow the "< 1 ms simple render" budget. So internally, `render` and `render_inline` build a **`Builder`**: a growable byte buffer.

`Builder` is implemented as a 4 KB initial buffer, doubling on growth (like `ObjList`). The final string is materialized once at the end of the render. All string concatenations in compiled view code go through `Builder`, not through `String`.

The view DSL helpers (`h1`, `a`, `div`, `form`, etc.) take the current `Builder` as an implicit first argument. The compiler emits the builder-passing convention; user code is unchanged.

```c
// Pseudocode for the compiled body of a small view
void view_home(VM *vm, Builder *b, Value posts) {
  builder_write_str(b, "<h1>Posts</h1><ul>");
  for (size_t i = 0; i < list_len(posts); i++) {
    Value post = list_get(posts, i);
    builder_write_str(b, "<li><a href=\"/posts/");
    builder_write_int(b, value_int(post, "id"));
    builder_write_str(b, "\">");
    builder_write_escaped(b, value_str(post, "title"));  // HTML-escaped
    builder_write_str(b, "</a></li>");
  }
  builder_write_str(b, "</ul>");
}
```

`Builder` is **not** exposed to user code. There is no `Builder.new`, no `b.append`. If a user function (e.g. a custom helper) needs to assemble a string in a tight loop, it should use `String.concat` — which is fine because user helpers are not in the hot path of view rendering. View rendering is the only place we do enough appends for the buffer to matter.

Escaping (`builder_write_escaped`) handles the five XML-significant characters: `& < > " '`. It does not touch anything else, so passing pre-escaped HTML through `raw "..."` is safe.

---

## 9. The Built-In 25-30 Stdlib Packages

Exactly these. No more, no less, in v1. If you need a 31st, you write it in vek.

| # | Package    | Purpose |
|---|------------|---------|
| 1 | `db`       | SQLite (default) + Postgres. Queries, transactions. |
| 2 | `kv`       | In-memory key-value, LRU bounded. |
| 3 | `cache`    | TTL cache with `get_or_set`. |
| 4 | `http`     | HTTP client (timeouts, retries, JSON helpers). |
| 5 | `json`     | `json.encode`, `json.decode`. |
| 6 | `form`     | Form parsing + validation DSL. |
| 7 | `csrf`     | Token generation, validation, helpers. |
| 8 | `session`  | Signed cookie sessions, server-side store. |
| 9 | `auth`     | `hash_password`, `verify_password`, `random_token`. |
| 10| `flash`    | One-shot session messages. |
| 11| `mail`     | SMTP send, plain + HTML, templates. |
| 12| `jobs`     | Background job queue, multiple workers. |
| 13| `storage`  | S3-compatible blob storage, presigned URLs. |
| 14| `log`      | Structured logging (text dev, JSON prod). |
| 15| `env`      | Env var access with defaults and required-flag. |
| 16| `time`     | `now`, `format`, `parse`, durations, `cron_next`. |
| 17| `uuid`     | `v4`, `v7`. |
| 18| `crypto`   | `sha256`, `hmac_sha256`, `constant_time_eq`, `random_bytes`. |
| 19| `path`     | URL path join/split/normalize. |
| 20| `ratelimit`| Token bucket per key. |
| 21| `compress` | gzip + brotli response encoding. |
| 22| `websocket`| Per-connection handlers, rooms, broadcast. |
| 23| `i18n`     | In-memory translations, simple key lookup. |
| 24| `webhook`  | Signature verification (GitHub, Stripe, generic). |
| 25| `markdown` | Markdown → safe HTML. |
| 26| `sanitize` | HTML allowlist sanitization. |
| 27| `csp`      | Content-Security-Policy builder. |
| 28| `slug`     | Slugify strings for URLs. |
| 29| `cors`     | CORS preflight + headers. |
| 30| `cli`      | Read stdin, parse args, ANSI colors for CLI tools. |

**That's 30.** All built in. No registry. No `import`. All available in every `.ve` file automatically.

### 9.1 What Each Package Looks Like (Selected)

```ruby
# db
db :sqlite, path: "data/app.db"             # configured in app.ve
posts = db.query("select * from posts")
one   = db.row("select * from posts where id = ?", 42)
n     = db.scalar("select count(*) from posts")
id    = db.exec("insert into posts (title) values (?)", "x")
db.transaction do
  db.exec(...)
  db.exec(...)
end
```

```ruby
# kv
kv.set "user:#{id}", u, ttl: 60
u = kv.get "user:#{id}"
kv.delete "user:#{id}"
kv.clear
```

```ruby
# cache
result = cache.get_or_set "user:#{id}:posts", ttl: 300 do
  db.query("select * from posts where user_id = ?", id)
end
```

```ruby
# http
resp = http.get("https://api.example.com/x", headers: {"auth" => tok}, timeout: 5)
case resp.status
in 200..299
  json.decode(resp.body)
in _
  raise "upstream: #{resp.status}"
end
```

```ruby
# session
session.set "user_id", user.id
session.get "user_id"     # int or nil
session.delete "user_id"
session.flash[:notice] = "saved"
```

```ruby
# jobs
jobs.enqueue "send_email", to: u.email, template: "welcome"
job "send_email" do |args|
  mail.send to: args.to, subject: "Welcome", body: "..."
end
```

```ruby
# storage
storage.put "avatars/#{u.id}.jpg", bytes, content_type: "image/jpeg"
url = storage.url "avatars/#{u.id}.jpg", expires: 3600
data = storage.get "avatars/#{u.id}.jpg"
```

```ruby
# log
log.info "user created", id: u.id
log.error "db failed", error: e.message
log.debug "request", method: req.method
```

```ruby
# form
form.validate(req.body) do
  field "email",    type: :email, required: true
  field "password", type: :string, min: 8, required: true
end
form.valid?      # bool
form["email"]    # string or nil
form.errors      # list of {field, message}
```

```ruby
# time
time.now                       # int epoch seconds
time.format(t, "%Y-%m-%d")     # string
time.parse("2026-01-01")       # int epoch or nil
60.seconds, 5.minutes, 1.day
time.cron_next("0 9 * * *")    # next epoch matching cron
```

```ruby
# env
env("DATABASE_URL", default: "sqlite:data/app.db")
env.required("SESSION_SECRET")   # raises at boot if missing
```

```ruby
# websocket
ws "/chat" do |conn|
  on message do |msg|
    broadcast msg, to: "chat_room"
  end
  on close do
    leave_room conn
  end
end
```

---

## 10. Persistence: SQLite, Postgres, KV

### 10.1 SQLite (default)

Why default: zero ops. Single file. Perfect for a $5 VPS running a side project.

- Driver: built into vek (no separate package).
- Connection mode: WAL.
- Connection pool size: configurable, default `min=2, max=10`.
- Migrations: plain `.sql` files in `migrations/`, applied in lexical order, recorded in a `_migrations` table.
- Backups: vekd snapshots the SQLite file nightly to `/var/lib/vek/apps/<name>/backups/`, keeps last 7.

### 10.2 Postgres (optional)

Configured with `db :postgres, url: env("DATABASE_URL")`. Same `db.query / db.row / db.scalar / db.exec / db.transaction` API.

Differences from SQLite:
- Uses `$1, $2, ...` placeholders (translated automatically by vek).
- Returns `BIGINT` as int. (SQLite returns `INTEGER` as int.)
- Supports `LISTEN/NOTIFY` (used by `jobs` for cross-worker wakeup in v1 if multiple workers are on the same host — v1 keeps it simple and uses polling with SQLite).

### 10.3 KV (in-memory)

- Bounded LRU map. Configurable `max_entries` and `max_bytes`.
- Optional `ttl` per entry (lazy expiry on read; periodic sweep every 60s).
- Optional persistence to a file (off by default, on for cross-process sharing within one app).
- Cross-process: not atomic. If you need atomicity, use `db`.

### 10.4 No Central Registry

There is no `vek add some-lib`. There is no `pub.vek-lang.org`. If you need a Postgres driver, it's there because we shipped it. If you need a Redis client, write one in vek using `tcp` (a v2 internal package not exposed to user code) or vendor the C source and call via… no, v1 has no FFI. So write a Redis client in pure vek, we don't care.

---

## 11. Compiler Pipeline & Bytecode Format

### 11.1 Pipeline

```
source .ve
  →  tokenizer
  →  parser          → AST
  →  resolver        → resolved AST (free vars, captures)
  →  type checker    → annotated AST (advisory only)
  →  optimizer       → constant fold, dead branch elim, inlining
  →  code generator  → bytecode
  →  packager        → .vebc file
```

`vek dev` keeps the source around and re-runs this on save. `vek build` runs the full pipeline and discards source from the artifact.

### 11.2 Bytecode (`.vebc`)

A `.vebc` file is a single binary artifact. Sections, in order:

```
Header
  magic:    4 bytes   "VEBC"
  version:  u16       (= 1 for v1)
  flags:    u16
  sha256:   32 bytes  of everything below
  reserved: 24 bytes

Constants table
  u32 count
  count × {
    tag:     u8        (int, float, string, bytes, func_ref)
    payload: variable
  }

Strings table
  u32 count
  count × {
    length:  u32
    data:    bytes (UTF-8, no null terminator)
  }

Function table
  u32 count
  count × {
    name_idx:     u32     (index into strings table)
    num_regs:     u16
    num_params:   u8
    num_upvalues: u8
    code_offset:  u32     (into instruction section)
    code_length:  u32
    line_table_offset:  u32
    line_table_length: u32
  }

Upvalue table (for closures)
  u32 count
  count × {
    src_idx:  u32     (function index this upvalue came from)
    slot:     u8
    is_local: u8
  }

Instruction section
  u32 total length in bytes
  raw bytecode (op + operands, variable length per op)

Line table
  u32 count
  count × {
    code_offset: u32
    source_line: u32
  }

Asset section
  u32 count
  count × {
    path:    u32   (string index)
    length:  u32
    data:    bytes
  }
```

This is small and mmap-able. A trivial app is < 50 KB. A medium app withored markdown vend is < 1 MB.

---

## 12. Virtual Machine

### 12.1 Register-Based

Each function has a fixed-size register window. Instructions name registers by small integer index (0-255). This dramatically reduces dispatch overhead compared to stack machines because:
- operands are explicit (no PUSH/POP pairs),
- register allocation can keep hot values in CPU registers (via a global register array the C compiler can reason about),
- one instruction does what would be three or four in a stack VM.

Example: `a = b + c * d` in stack VM is `LOAD b, LOAD c, LOAD d, MUL, ADD, STORE a` (6 ops). In register VM: `MUL r_c, r_d, r_t1; ADD r_b, r_t1, r_a` (2 ops).

### 12.2 NaN Boxing

A 64-bit `Value` is encoded so that doubles, ints, pointers, and a few special values all fit in 8 bytes and require no tag check in the common case.

Scheme (revised from earlier drafts):

| Top 16 bits       | Meaning                                          |
|-------------------|--------------------------------------------------|
| `0x0000..0xFFFB`  | IEEE 754 double (regular float)                  |
| `0x7FF8`          | Pointer (low 47 bits; 16-byte aligned)           |
| `0x7FF9`          | Integer (low 48 bits, signed)                    |
| `0x7FFA_0000_0000_0000` | `false`                                   |
| `0x7FFA_8000_0000_0000` | `true`                                    |
| `0x7FFB_0000_0000_0000` | `nil`                                     |
| `0x7FFB_8000_0000_0000` | `undefined` / unit                       |
| `0x7FFC_xxxx_xxxx_xxxx` | Heap object handle (for strings > 4 GB; n/a in v1) |
| `0x7FFD..0x7FFF`  | Reserved                                         |

Decoding rules:
- `is_float(v)` = `(v & 0xFFF8_0000_0000_0000) != 0x7FF8_0000_0000_0000` — i.e. not a non-float tag. **Allows doubles without any tag check for arithmetic.**
- `is_int(v)`: top 16 are `0x7FF9`.
- `is_ptr(v)`: top 16 are `0x7FF8`, and low 3 bits are zero (alignment check).
- `is_special(v)`: top 16 are `0x7FFA` or `0x7FFB`.

**Correctness of the fast path:** the previous draft claimed "no tag check, no branch" for arithmetic. That is true **only when both operands are already known to be floats** (e.g. the result of a previous float operation, or a literal float loaded from the constants table). The compiler must track the type of each register, and `OP_ADD_INT` / `OP_ADD_FLOAT` / `OP_ADD` (generic) are **separate opcodes** the type-inference pass emits:

- `OP_ADD_FLOAT` — no tag check, just `vaddsd`. Used when both operands are statically known to be `float`.
- `OP_ADD_INT` — no tag check, plain 48-bit int add. Used when both operands are statically known to be `int`.
- `OP_ADD` — generic opcode. Falls back to a tag check per operand, then dispatches to the float or int path.

The hot loop is the typed variants. The generic opcode is only emitted at value-type joins (e.g. after a conditional, where one branch is `int` and the other is `float`). For the common arithmetic-heavy code — numeric work, counters, pagination, averages — the compiler emits only `OP_ADD_INT` / `OP_ADD_FLOAT` and the dispatch stays in the no-branch regime.

Pseudocode for the float fast path:
```c
op_add_float: {
  // both operands are statically known to be float; no tag check.
  double a = AS_DOUBLE(r[ip[0]]);
  double b = AS_DOUBLE(r[ip[1]]);
  r[ip[2]] = AS_VALUE(a + b);
  ip += 3; DISPATCH();
}
op_add: {
  // generic; one tag check per operand.
  Value av = r[ip[0]], bv = r[ip[1]];
  if (IS_INT(av) && IS_INT(bv)) {
    r[ip[2]] = INT_VAL(AS_INT(av) + AS_INT(bv));
  } else {
    r[ip[2]] = AS_VALUE(AS_DOUBLE(av) + AS_DOUBLE(bv));
  }
  ip += 3; DISPATCH();
}
```

This is the same trick LuaJIT and Wren use, with the same caveat: the fast path is only fast when the compiler can prove the types. The type-inference pass is what makes the fast path real. Without it, the no-tag-check claim is just marketing.

### 12.3 Computed GOTO Dispatch

On GCC/Clang:

```c
static Value run(VM *vm) {
  uint8_t *ip = vm->frames[vm->nframes - 1].ip;
  Value *r   = vm->frames[vm->nframes - 1].regs;

  static const void *dispatch[] = {
    [OP_NOP]      = &&op_nop,
    [OP_LOAD_CONST] = &&op_load_const,
    [OP_MOVE]     = &&op_move,
    [OP_ADD]      = &&op_add,
    [OP_SUB]      = &&op_sub,
    /* ... */
  };

#define DISPATCH() do { op = *ip++; goto *dispatch[op]; } while (0)
  uint8_t op;
  DISPATCH();

op_nop:        DISPATCH();
op_move:       r[ip[0]] = r[ip[1]]; ip += 2; DISPATCH();
op_load_const: r[ip[0]] = vm->constants[u32_at(ip+1)]; ip += 5; DISPATCH();
op_add: {
  double a = AS_DOUBLE(r[ip[0]]);
  double b = AS_DOUBLE(r[ip[1]]);
  r[ip[2]] = AS_VALUE(a + b);
  ip += 3; DISPATCH();
}
/* ... */
}
#undef DISPATCH
}
```

This compiles to a jump table + indirect branch — the CPU's branch predictor learns the op stream after a few iterations and the inner loop runs ~1 op per cycle on modern x86. For MSVC (no computed goto), fall back to a `switch` with `__builtin_expect` hints.

### 12.4 Bytecode Opcodes (v1)

Approximately 60 opcodes. Skeleton:

```
OP_NOP
OP_HALT
OP_MOVE       ra, rb
OP_LOAD_CONST  ra, const_idx
OP_LOAD_NIL    ra
OP_LOAD_TRUE   ra
OP_LOAD_FALSE  ra
OP_LOAD_INT    ra, imm_i32        (small int that fits in 32 bits)
OP_LOAD_FLOAT  ra, imm_f64        (rare; usually via constants)
OP_LOAD_GLOBAL ra, name_idx
OP_STORE_GLOBAL ra, name_idx
OP_GET_FIELD   ra, obj_rb, name_idx
OP_SET_FIELD   obj_ra, name_idx, val_rb
OP_GET_INDEX   ra, list_rb, idx_rc
OP_SET_INDEX   list_ra, idx_rb, val_rc
OP_ADD ra, rb, rc        (float fast path; int + string variants for typing)
OP_SUB ra, rb, rc
OP_MUL ra, rb, rc
OP_DIV ra, rb, rc
OP_MOD ra, rb, rc
OP_NEG ra, rb
OP_EQ  ra, rb, rc
OP_NEQ ra, rb, rc
OP_LT  ra, rb, rc
OP_LTE ra, rb, rc
OP_GT  ra, rb, rc
OP_GTE ra, rb, rc
OP_NOT ra, rb
OP_JUMP          offset
OP_JUMP_IF_FALSE ra, offset
OP_JUMP_IF_TRUE  ra, offset
OP_CALL          ra, argc, ...
OP_TAILCALL      ra, argc
OP_RETURN        ra
OP_CLOSURE       ra, func_idx
OP_GET_UPVALUE   ra, uv_idx
OP_SET_UPVALUE   uv_idx, ra
OP_NEW_LIST      ra, count
OP_NEW_MAP       ra, count
OP_NEW_BYTES     ra, count
OP_FOR_PREP      ra, offset       (range iter)
OP_FOR_NEXT      ra, offset
OP_ITER_NEW      ra, src_rb       (list iter)
OP_ITER_NEXT     ra, rb, offset   (rb has key/value pair)
OP_THROW         ra              (raises)
OP_PUSH_HANDLER  offset
OP_POP_HANDLER
OP_GET_LOCAL     ra, slot
OP_SET_LOCAL     slot, ra
OP_DUP           ra
OP_POP           ra
OP_CLOSE_UPVALUE ra
OP_IMPORT        name_idx        (resolves stdlib package; no-op at runtime if already loaded)
```

### 12.5 Calls & Frames

```c
typedef struct {
  ObjClosure *closure;
  uint8_t   *ip;
  Value      *regs;       // points into the function's register window
  uint16_t    reg_count;
} CallFrame;
```

Frames live in a fixed-size array on the VM (default 256, configurable). Register windows are allocated in a `Value regs[65536]` array on the VM. Overflowing either is a panic in v1; v2 may grow the frame array.

Tail calls reuse the current frame. Upvalues are closed when a function returns; if an outer function still references a closed-up local, it's promoted to the heap.

### 12.6 Calling C (Built-ins)

```c
typedef Value (*BuiltinFn)(VM *vm, int argc, Value *argv);

Value builtin_db_query(VM *vm, int argc, Value *argv) {
  // argv[0] = sql string, argv[1..] = bound params
  ...
}
```

The dispatch in `OP_CALL` checks the function's `is_builtin` flag and calls directly. No C stack frame for the bytecode interpreter, just a function call. Built-ins are free to do anything (block on I/O via the event loop hook).

---

## 13. Garbage Collector

### 13.1 Mark-and-Sweep, Stop-the-World

v1. Simple. Reliable. No tuning knobs. We will not pretend incremental or generational is free.

Phases:
1. **Stop the world** at a safepoint (function call, loop backedge, allocation over threshold).
2. **Mark roots**: all global values, all stack slots in all frames, all upvalues reachable from open frames, all handles held by C code.
3. **Mark transitively** following pointer values.
4. **Sweep** all heap pages: unmarked objects are freed; marked ones are unmarked for next cycle.
5. **Resume**.

### 13.2 Heap Layout

- Heap is a linked list of 16 KB pages.
- Each page has a header with: `first_free_offset`, `free_list_head` (when in free-list mode), `sweep_cursor`.
- Bump allocation by default. When a page fills, switch to free-list mode. After a sweep, reset to bump mode if empty.
- Each object starts with `ObjHeader`:

```c
typedef struct {
  uint8_t  type;       // OBJ_STRING, OBJ_LIST, etc.
  uint8_t  flags;      // bit 0: mark, bit 1: pin, bit 2: large
  uint32_t size;       // total bytes including header
  uint32_t hash;       // for strings
  struct Page *page;   // back-pointer for sweep
} ObjHeader;
```

### 13.3 Trigger Policy

- Trigger GC when `bytes_allocated_since_gc > bytes_alive_after_last_gc * 2` (the classic 2x heap heuristic).
- Also trigger if `bytes_alive > hard_limit` (defaults to 256 MB per app, configurable in `app.ve`).

### 13.4 Pins

C code can `vm_pin(v)` to prevent collection. Useful during a `db.query` call that returns a row list — pin it for the duration of the user-visible call.

### 13.5 What v1 Does Not Have

- No generational GC.
- No incremental / concurrent GC.
- No compacting GC.
- No finalizers (`__gc` / `defunct`). If you need cleanup, do it in an `on shutdown` hook in `app.ve`.

These are v2+ concerns. A web app doing 100 req/s of 1 KB responses should allocate ~100 KB/s of garbage, which is trivial for a 2x-heap mark-sweep.

---

## 14. String, List, Map Internals

### 14.1 Strings

```c
typedef struct {
  ObjHeader header;
  uint32_t  length;        // bytes (UTF-8)
  uint32_t  hash;
  char      data[];        // null-terminated for C interop
} ObjString;
```

- Immutable. UTF-8.
- Interned: identifier-like strings (lowercase, no spaces, < 64 chars) are interned at parse time and on first `kv.set` / `req.query` access. Comparison is pointer-equality.
- Length-prefixed: `s.length` is `O(1)`.
- `s[i]` returns a single-character string (allocates).
- Slicing: `s.slice(start, end)` returns a new string; v1 doesn't share memory (no rope).
- Concatenation: `s.concat(other)` allocates a new string. The compiler's `#{interp}` lowers to `concat`.

Methods (v1): `length`, `slice`, `upper`, `lower`, `trim`, `starts_with`, `ends_with`, `contains`, `split`, `replace`, `to_i`, `to_f`, `to_bytes`, `bytes`, `chars`, `index_of`, `repeat`, `pad_left`, `pad_right`, `is_empty`, `==`, `!=`.

### 14.2 Lists

```c
typedef struct {
  ObjHeader header;
  uint32_t  length;
  uint32_t  capacity;
  Value    *data;
} ObjList;
```

- Dynamic array. Doubles on growth.
- `O(1)` amortized append, `O(1)` random access, `O(n)` insert/remove in middle.
- Iteration: `for x in list` uses `OP_ITER_NEW` / `OP_ITER_NEXT`.

Methods: `length`, `is_empty`, `first`, `last`, `push`, `pop`, `shift`, `unshift`, `insert`, `remove`, `slice`, `map`, `filter`, `reduce`, `find`, `any?`, `all?`, `sort`, `sort_by`, `reverse`, `join`, `contains`, `uniq`, `flatten`, `zip`, `each`, `each_with_index`.

### 14.3 Maps

```c
typedef struct {
  ObjHeader header;
  uint32_t  length;
  uint32_t  capacity;       // power of 2
  MapEntry *entries;        // open-addressing
} ObjMap;
```

- Open-addressing with linear probing. Tombstones for delete.
- String keys only in v1. (Maps with non-string keys would need a different equality story.)
- Insertion-ordered: we also keep a parallel `entries_ordered` list (or use a linked list within the table) so `for k, v in map` is in insertion order.
- SmallMap optimization: maps with ≤ 4 entries use a flat `Value[4]` for keys and values; promoted to hash table on growth.

Methods: `length`, `is_empty`, `get`, `set`, `delete`, `has`, `keys`, `values`, `entries`, `merge`, `each`, `map`, `filter`.

### 14.4 Bytes

```c
typedef struct {
  ObjHeader header;
  uint32_t  length;
  uint8_t   data[];
} ObjBytes;
```

For binary data: file uploads, crypto output, S3 blobs. Indexable as ints.

### 14.5 Why No `Set`

Use `map` with `true` values. Saves us a type. If a `Set` becomes a perf problem, we add it in v2.

---

## 15. Concurrency & I/O

### 15.1 The Model: One Process, Fibers on the Main Thread, Workers for Blocking I/O

The previous draft of this section said "single thread, event loop, code runs to completion." That was wrong: if `db.query` blocks the main thread for 50 ms, every other request stalls. This section fixes that.

The actual model has three pieces, in one process:

1. **One main OS thread** running the VM interpreter, the fiber scheduler, and the event loop (epoll).
2. **A pool of N worker OS threads** for blocking I/O. Default `N = min(num_cores, 16)`, configurable in `app.ve` as `io_threads: N`.
3. **A set of fibers** (cooperative coroutines, one per in-flight unit of work) scheduled on the main thread.

```
   ┌─────────────────── main thread ─────────────────────┐
   │                                                      │
   │  event loop:                                         │
   │    epoll_wait()                                      │
   │      ├─ I/O completion from worker pool → resume F  │
   │      ├─ timer fired → resume cron/interval F         │
   │      ├─ accept() new conn → spawn new fiber          │
   │      └─ runloop drained → wait                       │
   │                                                      │
   │  fiber scheduler (FIFO run queue):                   │
   │    while run-queue not empty:                        │
   │      pop fiber F                                     │
   │      resume F until it suspends or finishes          │
   │                                                      │
   └──────────────────────────────────────────────────────┘
                              │ post job (lock-free MPSC ring)
                              ▼
   ┌─────────────── worker thread pool ──────────────────┐
   │  T1: db.query(sql, params) → rows                   │
   │  T2: http.get(url) → response                       │
   │  T3: file.read(path) → bytes                        │
   │  T4: ssl_handshake + ws upgrade                      │
   │  ...                                                │
   │  on complete: write result to job desc,             │
   │                write byte to main thread's eventfd   │
   └──────────────────────────────────────────────────────┘
```

**User code is unchanged.** `rows = db.query("...")` still looks like a synchronous call. The fiber just suspends, the I/O runs on a worker, and the fiber resumes when the result is ready.

### 15.2 Fibers

A fiber is a cooperatively-scheduled coroutine. It has:
- A C stack (64 KB initial, grows on demand up to 1 MB).
- A saved execution context: instruction pointer, register window, upvalue list.
- A state: `READY` / `RUNNING` / `SUSPENDED` / `DEAD`.

One fiber per in-flight unit of work:
- **One fiber per HTTP request.** Born at `accept`, dies at response complete (or at `Unwind`, see §4.15).
- **One fiber per WebSocket connection.** Lives for the connection's lifetime.
- **One fiber per active job worker slot.**
- **One fiber per cron/interval timer.**

Fibers are **cooperatively** scheduled. There is no preemption. A fiber runs until it:
- Returns from its root function (the request handler finishes).
- Calls a built-in that suspends (any I/O).
- Calls the explicit `yield` built-in (v1: may not exist; v2: yes).

**No "yield opcode" in the bytecode.** The VM's interpreter loop runs to completion per fiber resume. The fiber as a whole suspends — its C-level state is saved by swapping the stack pointer and a few callee-saved registers, not by walking the bytecode.

The context-switch implementation: `makecontext` / `swapcontext` (POSIX) for portability, or a hand-rolled assembly trampoline on Linux for speed. Either way the cost is ~200 ns per suspend/resume pair.

### 15.3 How a Blocking I/O Call Works

User code:

```ruby
rows = db.query("select * from posts where user_id = ?", uid)
```

Runtime, step by step:

1. `OP_CALL db.query` dispatches to the C built-in.
2. Built-in marshals args (sql string, int param) into a heap-allocated `Job` descriptor.
3. Built-in pushes the `Job` onto the worker pool's MPSC ring buffer.
4. Built-in returns the sentinel `VM_SUSPEND`.
5. The VM sees `VM_SUSPEND` from the call. It calls `vm_save_fiber(f)`, which stores the fiber's C stack pointer, IP, and register window.
6. The fiber is marked `SUSPENDED` and parked on a list keyed by `Job.id`.
7. The scheduler pops the next `READY` fiber and resumes it. (Or, if none, calls `epoll_wait`.)
8. ... other fibers run, time passes ...
9. Worker thread completes the query. It writes the result rows into the `Job` descriptor, writes 1 byte to the main thread's `eventfd`.
10. Main thread's `epoll_wait` wakes up. The I/O-completion handler reads the `Job.id`, looks up the parked fiber, marks it `READY`, and enqueues it on the run queue.
11. Eventually the scheduler pops the fiber. It restores its C stack, registers, and IP.
12. The built-in "returns" with the result, which the VM materializes as a normal `Value` on the fiber's register window.
13. Execution continues from the instruction after the call. The user code sees `rows` as a regular local.

**Per-call overhead:** ~200 ns of fiber save/restore, plus the cost of the syscall. The user code is byte-for-byte identical to a synchronous implementation. No `await`. No callbacks. No function coloring.

### 15.4 What Runs Where

Main thread does anything under ~100 µs. The cost of cross-thread signaling (eventfd + scheduler enqueue) is ~5-10 µs; if the work is faster than that, do it inline.

| Operation                                          | Thread               |
|----------------------------------------------------|----------------------|
| All user bytecode                                  | Main                 |
| View rendering                                     | Main                 |
| `db.query` / `db.exec` / `db.scalar` / `db.row`    | Worker               |
| `db.transaction` (the open/commit/fsync)           | Worker               |
| `http.get` / `http.post` / `http.put`              | Worker               |
| `storage.get` / `storage.put` (S3 / local)         | Worker               |
| `mail.send`                                        | Worker               |
| `file.read` (small, < 4 KB)                        | Main (inline)        |
| `file.read` (large) / `file.write`                 | Worker               |
| `crypto.sha256` / `hmac_sha256` / `random_bytes`   | Main (< 100 µs)      |
| `crypto.pbkdf2` (slow by design)                   | Worker               |
| `json.encode` / `json.decode`                      | Main                 |
| `log.*`                                            | Main (writes to lock-free buffer; flushes async) |
| `env("X")`                                         | Main                 |
| `time.now`                                         | Main                 |
| `kv.*`                                             | Main (in-memory)     |
| `cache.*`                                          | Main                 |
| `form.validate`                                    | Main                 |
| `session.get` / `session.set` (client-only cookie) | Main                 |
| `session.*` (server-side store)                    | Worker               |
| `csrf.*`                                           | Main                 |
| `ratelimit.check`                                  | Main                 |
| `uuid.v4`                                          | Main                 |
| `csp.*` / `cors.*`                                 | Main                 |
| `slug.generate`                                    | Main                 |
| `sanitize.html` / `markdown.render`                | Main                 |
| WebSocket frame I/O                                | Worker (raw I/O) + main (frame dispatch) |

If a "Worker" operation ever turns out to take > 100 µs, the worker pool is the bottleneck, not the main thread.

### 15.5 Worker Thread Pool Sizing and Backpressure

- Default: `min(num_cores, 16)`.
- Configurable: `app.ve` can set `io_threads: N`.
- Best-effort pinning via `pthread_setaffinity_np` (Linux).
- If a job takes > 30 s, vekd logs a slow-I/O warning.
- Jobs over 5 min are killed; the fiber is resumed with `DbTimeoutError` / `HttpTimeoutError` / `IoTimeoutError`. Caller can `rescue`.
- If the worker pool's queue is full (default cap: 4 × pool size), new jobs are enqueued with backpressure — the calling fiber suspends until queue space frees up. This is invisible to user code; it just means a slow DB will eventually slow down the request rate, not OOM the process.

### 15.6 Multi-Core: Multiple Processes

This is unchanged. To use more than one core, run multiple vek processes for the same app. vekd's reverse proxy load-balances across them with round-robin or `least-connections`.

- An app on 4 cores = 4 vek processes, each with its own VM, its own fiber scheduler, its own worker pool.
- No shared in-memory state across processes. Use `db` (SQLite/Postgres) or shared `kv` with file persistence for cross-process state.
- A crash in one worker doesn't take the others down.

This is the right trade-off for the kind of app vek targets. Real OS-thread parallelism inside a single process is a v2 problem (and it will need: safepoints in the bytecode loop, a preemption timer signal, a worker-pool-aware scheduler). v1 ships faster this way.

### 15.7 I/O Backends

- **Linux**: epoll, edge-triggered. Direct implementation, no libuv.
- **macOS / BSD** (for dev): kqueue.
- **No Windows** in v1.

We implement epoll directly rather than depending on libuv. Reasons:
- ~400 lines of C.
- One less thing to vendor and update.
- Full control over timer wheels, accept storms, etc.
- libuv's design (handle-per-resource) is wasteful for our needs.

### 15.8 Why This Model, Not Callbacks or async/await

**Callbacks** (Node pre-async/await): leads to "callback hell" and forces every I/O to be async at the language level. Bad.

**`async`/`await`** (modern Node, C#, Rust): works, but introduces parallel function coloring — `async` functions can't be called from sync ones without blocking. A user calling a "sync-looking" function never knows if it'll block the event loop.

**Transparent fibers** (this design): user writes `db.query("...")` and gets the result. No color. No callbacks. The cost is a worker thread pool and a fiber swap per I/O call (~200 ns), which is in the noise compared to the syscall itself. The user never has to think about it.

### 15.9 What This Model Does Not Give You

- **No CPU parallelism in one process.** A tight `for` loop computes on the main thread. Run more processes for CPU-bound work.
- **No parallelism within a single request.** `db.query` and `http.get` are sequential. To parallelize I/O within a request, fire the calls into `jobs` and join (v2). v1: write sequential.
- **No preemption.** A fiber doing `1_000_000.times { compute() }` will hog the main thread. v2 adds a `yield` built-in and a preemption timer. v1: don't write that loop. If you must, break it into chunks and `return` to the event loop.
- **No thread-local storage.** All state is per-fiber (cleaner) or per-VM (globals). v1 has no `Thread.current`-style API.

---

## 16. Error Handling

### 16.1 Three Kinds of Failure

1. **Expected, recoverable** — db returned no row, form had a validation error, file didn't exist.
   → Return `nil` or a tagged value. Caller checks.

2. **Expected, programmatic** — JSON parse failed, integer overflow, type mismatch.
   → `raise` with a message. Caller `rescue`s or lets it bubble.

3. **Unexpected, fatal** — VM panic, OOM, assertion failure.
   → Print to stderr, dump core, exit. vekd marks the app as crashed and restarts.

### 16.2 Handler Errors

A `raise` in a handler is caught by the framework:
- If there's a `rescue` block, it runs.
- Otherwise, the response is a 500 with a generic message.
- The error is logged with the request ID.

### 16.3 Process Errors

A `raise` in `app.ve` boot or in a job worker is fatal. vekd restarts the process. If a job raises 3 times in a row, the job is moved to a `dead_jobs` table for inspection.

---

## 17. HTTP Server, Routing, Sessions, CSRF

### 17.1 HTTP Server

- Listens on the configured port (default 3000 for dev, internal port for prod).
- HTTP/1.1 with keep-alive.
- HTTP/2 in v2.
- TLS: not done by the app. vekd / Cloudflare terminates TLS.

### 17.2 Router

Trie-based, supports:
- Literal segments: `/about`
- Dynamic: `/posts/:id`
- Constrained: `/posts/:id(int)` / `/posts/:id(uuid)`
- Catch-all: `/files/*path`
- Method dispatch: trie per method.

Lookup is `O(depth)` with cheap comparisons (interned strings). 1000 routes is a few microseconds.

### 17.3 Sessions

```ruby
# app.ve
session :cookie, secret: env("SESSION_SECRET"), name: "sid"
```

Two storage modes:
- **Client-only** (default): the entire session is in a signed, encrypted cookie. ≤ 4 KB.
- **Server-side**: a session ID in the cookie, the data in SQLite. For larger sessions.

For v1, client-only is the default. Server-side is opt-in.

### 17.4 CSRF

- Every session has a `csrf_token`.
- Every form rendered via the `form` helper includes a hidden `_csrf` field.
- `POST` / `PUT` / `PATCH` / `DELETE` without a valid token → 403.
- Tokens are bound to the session, expire with it.

The `form` helper auto-injects the token. If you write raw HTML, use `csrf.tag`.

**Exemptions (important — the previous version of this section broke every JSON API and every webhook).**

CSRF is a defense against **cookie-authenticated cross-site requests**. It only applies when:
1. The request carries a session cookie, AND
2. The request is not from a trusted origin.

A request that authenticates by `Authorization: Bearer <token>` (i.e. your JSON API) or by a webhook signature (i.e. Stripe / GitHub) doesn't have a session cookie for CSRF to defend. The default rules:

- **Files under `pages/api/` are CSRF-exempt by default.** They typically use Bearer tokens, API keys, or webhook signatures — none of which are vulnerable to CSRF (the attacker would need the token, which they can't read cross-origin). If you put a session-cookie-authenticated route under `pages/api/`, declare `csrf on` in the file to opt back in.
- **Webhook routes are CSRF-exempt by default.** A webhook isn't initiated by a user browser at all — it's a server-to-server POST. Verify the webhook signature using the `webhook` package instead.
- **Per-route opt-out:** add `skip_csrf` inside a handler block:

```ruby
post "/api/third-party-callback" do
  skip_csrf
  # verify the caller's signature instead
  webhook.verify_github req, secret: env("GITHUB_WEBHOOK_SECRET")
  ...
end
```

- **Per-file opt-in (re-enable CSRF inside an api/ file):**

```ruby
csrf on

post "/api/admin-action" do
  # CSRF enforced
end
```

- **`cors` and CSRF:** the `cors` package can be configured to skip CSRF for specific `Access-Control-Allow-Origin` values. This is opt-in, requires an explicit allowlist, and is logged at boot. Don't use wildcard CORS + CSRF-skip together.

**Decision tree for new endpoints:**

| Endpoint type                  | Auth              | CSRF? |
|--------------------------------|-------------------|-------|
| HTML form, cookie session     | session cookie    | yes (default) |
| JSON API, Bearer token         | `Authorization`   | no (default in api/) |
| JSON API, cookie session       | session cookie    | `csrf on` to opt in |
| Webhook (Stripe, GitHub, etc.) | signature         | no (default; use `webhook.*`) |
| Public form (newsletter etc.)  | none / captcha    | `csrf on` or reCAPTCHA  |

### 17.5 Cookies

```ruby
cookies.set "pref", "dark", max_age: 365 * 86400, http_only: false
val = cookies.get "pref"
cookies.delete "pref"
```

### 17.6 Static Files

Served from `public/` at the URL root. ETag + Last-Modified. No directory listing. Cache-Control is configurable per file type in `vek.toml`.

### 17.7 Streaming Responses

```ruby
get "/big.csv" do
  response.stream do |out|
    db.query("select * from big_table") do |row|
      out.write csv_row(row)
    end
  end
end
```

Useful for big exports.

---

## 18. Jobs & Mail

### 18.1 Jobs

In-process queue with a configurable number of worker threads (in the same process). Persistent backing store in SQLite (or Postgres) so jobs survive process restarts.

```ruby
# app.ve
jobs queue: "default", workers: 4, store: :sqlite

# Define a job
job "send_welcome" do |args|
  user = db.row("select * from users where id = ?", args.user_id)
  mail.send to: user.email, subject: "Welcome", body: render("emails/welcome.ve", user: user)
end

# Schedule a job
jobs.enqueue "send_welcome", user_id: u.id                       # immediate
jobs.enqueue_at 5.minutes.from_now, "send_reminder", user_id: u.id
jobs.enqueue_in 1.hour,        "send_reminder", user_id: u.id
```

Job return values: ignored. Errors are logged, and the job is retried with exponential backoff: 30s, 2m, 10m, 1h, 6h. After 5 failures, moved to `dead_jobs`.

A separate `vek worker` mode is supported for splitting job workers onto a different process from the web server (use when jobs are CPU-heavy).

### 18.2 Mail

SMTP client. `mail.send_template` renders a vek view and emails the result.

```ruby
mail.send to: "u@example.com", subject: "Hi", text: "Hello"
mail.send to: "u@example.com", subject: "Hi", html: "<h1>Hello</h1>"
mail.send_template "emails/welcome.ve", to: u.email, vars: { user: u }
```

Templates can return either a string (for text), or a map `{text: ..., html: ...}`. v1 does not handle attachments (v2 with `mail.attach`).

---

## 19. Logging & Observability

### 19.1 Logging

`log.info`, `log.warn`, `log.error`, `log.debug`. Each call is a structured event:

```ruby
log.info "user created", id: u.id, email: u.email
# → {"ts":1704067200,"level":"info","msg":"user created","id":42,"email":"a@b.c"}
```

In dev: pretty-printed colored output.
In prod: one JSON object per line on stdout. vekd collects and streams to the UI.

### 19.2 Request Logging

Every request logs:
- method, path, status, duration_ms, ip, user_agent, request_id.

The `request_id` is a UUID v4 generated per request, included in `X-Request-Id` response header, and forwarded to the job queue when a job is enqueued from a request.

### 19.3 Metrics (v1: minimal)

- Counters: requests_total, jobs_processed_total, jobs_failed_total.
- Histograms: request_duration_ms, db_query_duration_ms.
- Exposed at `/__metrics__` (off by default; turn on with `metrics on: true` in `app.ve`).

Output is Prometheus text format. Ship to your own Prometheus if you want. We don't run one for you.

### 19.4 Health Endpoints

- `GET /__health__` → 200 if alive, 500 if not (rare; usually just process is down).
- `GET /__ready__` → 200 if dependencies (db) are reachable.

These are used by vekd for crash detection and by Cloudflare / load balancers.

---

## 20. CLI (`vek`)

```
vek new APP_NAME              # create new project
vek dev                       # local dev server, hot reload
vek build [path]              # produce .vebc in build/
vek run [path]                # run a .vebc (or source in dev)
vek repl                      # interactive REPL
vek shell                     # REPL with app.ve loaded (db, helpers, etc.)
vek fmt [paths]               # format .ve files in place
vek check [paths]             # parse + advisory type check
vek migrate [up|down|status|new NAME]
vek test [paths]              # run *.test.ve files
vek version
vek help [command]
```

### 20.1 `vek new`

Scaffolds a new project. Asks for app name, db (sqlite/postgres), and a starter ("blank", "blog", "auth-example").

### 20.2 `vek dev`

- Watches `app.ve`, `pages/`, `views/`, `migrations/`, `public/`.
- On change, re-parses affected files. Incremental: doesn't recompile unrelated files.
- Serves on `127.0.0.1:3000` by default. Override with `--port` or `PORT` env var.
- Pretty error pages with source position, stack trace, locals.
- Formatter: auto-formats on save if `--format` is passed.

### 20.3 `vek build`

- Compiles `app.ve` + all `.ve` files in `pages/`, `views/`, `migrations/`, `public/`.
- Produces `build/app.vebc` (or custom path).
- Embeds all `public/` files into the asset section.
- Strips source positions from the bytecode (keeps line table for stack traces only if `--with-lines`).

### 20.4 `vek run`

- Loads `build/app.vebc` (or source if `--dev`).
- Starts the HTTP server.
- Configurable with `--port`, `--host`, `--workers` (multi-process).

### 20.5 `vek fmt`

- Opinionated formatter. Like `gofmt`. Indent: 2 spaces. Max line: 100 chars.
- Indent-based, not column-based, so it agrees with most editors.
- No config file. If you disagree, write a wrapper.

### 20.6 `vek repl` and `vek shell`

- Read-eval-print loop. Multi-line input (parens-balanced).
- `repl`: bare language, no app context.
- `shell`: loads `app.ve`, you have `db`, `kv`, `mail`, etc. in scope. Type `db.query("select ...")` and see rows.

### 20.7 `vek migrate`

```
vek migrate                    # apply all pending
vek migrate status             # show applied/pending
vek migrate new create_posts   # create migrations/NNN_create_posts.sql
```

`down` is not a thing. If you need to undo, write a new migration.

---

## 21. Dashboard (`vekd`)

A separate binary. Long-running. The thing that turns a fresh VPS into a deploy target.

### 21.1 Install

```bash
# on the VPS, as root or with sudo
curl -fsSL https://vek.sh/install.sh | sudo sh
# installs vekd to /usr/local/bin/vekd
# sets up /var/lib/vek/
# creates 'vek' system user
# starts vekd.service via systemd
```

After install, visit `http://VPS_IP:8080`, log in with the password printed at install time (and rotate it).

### 21.2 Architecture

```
               ┌──────────────────────────────────────────────┐
               │  vekd                                        │
   :80/:443    │                                              │
   ──────────► │  ┌─────────────┐    ┌──────────────────────┐ │
   reverse     │  │   proxy     │    │   supervisor         │ │
   proxy       │  │ (host hdr)  │    │   (per-app)          │ │
               │  └─────┬───────┘    └──────────┬───────────┘ │
               │        │                       │             │
               │        ▼                       ▼             │
               │  ┌─────────────────────────────────────────┐ │
               │  │  app table, deploys, users, settings    │ │
               │  │  (SQLite at /var/lib/vek/vekd.db)      │ │
               │  └─────────────────────────────────────────┘ │
               │                                              │
   :8080       │  ┌─────────────────────────────────────────┐ │
   ──────────► │  │  web UI (htmx)                          │ │
   admin       │  └─────────────────────────────────────────┘ │
               └──────────────────────────────────────────────┘
                            │           │           │
                            ▼           ▼           ▼
                       app 1 proc  app 2 proc  app 3 proc
                       cgroup      cgroup      cgroup
                       user        user        user
```

### 21.3 Data Model (vekd's own SQLite)

```sql
create table apps (
  id integer primary key,
  name text unique not null,
  repo_url text not null,
  branch text not null default 'main',
  pat_secret_ref text,        -- references secrets(id)
  domain text unique,
  port integer unique,        -- internal port
  cpu_weight integer default 100,
  memory_max integer default 536870912, -- 512 MB
  state text not null default 'pending', -- pending|building|healthy|stopped|crashed
  current_release text,
  created_at integer,
  updated_at integer
);

create table releases (
  id integer primary key,
  app_id integer references apps(id),
  ref text not null,           -- git sha
  artifact_path text,          -- /var/lib/vek/apps/<name>/releases/<ts>/
  created_at integer,
  deploy_log text
);

create table env_vars (
  app_id integer references apps(id),
  key text,
  value_encrypted blob,
  primary key (app_id, key)
);

create table events (
  id integer primary key,
  app_id integer references apps(id),
  kind text,                   -- deploy|start|stop|crash|restart
  message text,
  created_at integer
);

create table secrets (
  id integer primary key,
  name text unique,
  value_encrypted blob
);

create table users (
  id integer primary key,
  email text unique,
  password_hash text,
  totp_secret text,
  is_admin integer,
  created_at integer
);
```

`value_encrypted` is encrypted at rest with a key derived from a master key the admin sets at install time (held in `/var/lib/vek/master.key`, mode 0600).

### 21.4 CLI (`vekd`)

```bash
vekd init                    # first-time setup
vekd status                  # vekd status + app summary
vekd apps list
vekd apps add REPO PAT BRANCH
vekd apps deploy APP
vekd apps rollback APP RELEASE
vekd apps logs APP [-f]
vekd apps stop APP
vekd apps start APP
vekd apps restart APP
vekd apps delete APP
vekd apps env APP set KEY VAL
vekd apps env APP unset KEY
vekd users add EMAIL
vekd users set-password EMAIL
vekd users add-totp EMAIL
vekd cloudflare set-token TOKEN
vekd cloudflare add APP DOMAIN
vekd cloudflare purge APP
vekd backup                   # snapshot all apps' data + state
vekd restore BACKUP
vekd version
```

Most of these have web UI equivalents.

### 21.5 Web UI

HTML + htmx. No client-side framework. No build step. Pages are server-rendered vekd-internal vek apps themselves (dogfooding).

Pages:
- `/` — login
- `/dashboard` — app list, system resources
- `/apps/new` — deploy form
- `/apps/:id` — overview, env, deploys, logs, settings
- `/apps/:id/deploys/new` — trigger deploy
- `/apps/:id/logs` — tailing logs
- `/settings/users`
- `/settings/cloudflare`
- `/settings/backup`

### 21.6 Single-User, Multi-App

vekd's model: **one sysadmin** runs vekd. That person deploys many apps (their own + their clients').

For v1, auth is one user. Add more users in v2.

---

## 22. Deploy Flow End-to-End

A walk-through of pushing a new app to a fresh VPS.

### 22.1 One-Time Setup (on the VPS)

```bash
# 1. Install vekd
curl -fsSL https://vek.sh/install.sh | sudo sh

# 2. Visit http://VPS_IP:8080, log in with temp password, set new password
# 3. Optionally set Cloudflare API token in /settings/cloudflare
# 4. Optionally add an SSH key for git access (or use HTTPS PAT)
```

### 22.2 New App (via UI)

1. Click **New app**.
2. Fill form:
   - **Name**: `myblog`
   - **Repo**: `https://github.com/me/myblog`
   - **Branch**: `main`
   - **Git credential**: select or add (PAT or SSH key)
   - **Domain**: `myblog.com` (optional, requires Cloudflare)
   - **Env vars**: `SESSION_SECRET=…`, `DATABASE_URL=…`
   - **Resources**: `CPU: 1 core, RAM: 512 MB, PIDs: 256`
3. Click **Deploy**.

### 22.3 What vekd Does

```
1. Allocate port: 10001 (next free in 10000-19999)
2. Create cgroup: /sys/fs/cgroup/vek/myblog/
     memory.max = 536870912
     cpu.max    = "100000 100000"   (1 core)
     pids.max   = 256
3. Create user: vek_myblog
     useradd --system --shell /usr/sbin/nologin --home-dir /var/lib/vek/apps/myblog vek_myblog
4. Set cgroup ownership: chown -R vek_myblog:vek_myblog /sys/fs/cgroup/vek/myblog/
5. Create app dir: /var/lib/vek/apps/myblog/
     src/        (will clone here)
     releases/   (timestamped)
     data/       (app's SQLite etc.)
     log/        (app stdout/stderr)
     env         (env vars file, mode 0600)
     current → releases/<ts>/     (symlink)
6. Write env file: KEY=VAL per line
7. git clone --depth 1 --branch main https://x-access-token:PAT@github.com/me/myblog src
8. cd src && vek build --out /var/lib/vek/apps/myblog/releases/<ts>/app.vebc
9. chown -R vek_myblog:vek_myblog /var/lib/vek/apps/myblog
10. Update current symlink → releases/<ts>/
11. Start:
    systemd-run --unit=vek-myblog \
      --property=CPUWeight=100 \
      --property=MemoryMax=512M \
      --property=User=vek_myblog \
      --property=WorkingDirectory=/var/lib/vek/apps/myblog/current \
      --property=EnvironmentFile=/var/lib/vek/apps/myblog/env \
      --property=StandardOutput=append:/var/lib/vek/apps/myblog/log/app.log \
      --property=StandardError=append:/var/lib/vek/apps/myblog/log/app.err.log \
      --property=Restart=on-failure \
      --property=RestartSec=5s \
      /usr/local/bin/vek run /var/lib/vek/apps/myblog/current/app.vebc
    (or: cgexec + runuser if systemd-run unavailable)
12. Wait for /__ready__ to return 200 (up to 30s, else mark failed)
13. If domain set, call Cloudflare API:
     POST /zones/:zone/dns_records { type: A, name: myblog.com, content: VPS_IP, proxied: true }
14. Insert into `events` table, stream to UI
15. Roll back: if start fails, restore previous release symlink
```

### 22.4 Steady-State: Supervision

vekd has a supervisor goroutine (well, a vekd thread) that:
- Every 10s: polls `/__health__` of each app, records CPU% / RSS from cgroup.
- Every 30s: checks process liveness (PID exists in cgroup).
- On `systemd` failure: logs event, lets systemd restart (we configured `Restart=on-failure`).
- After 3 crashes in 60s: marks app as `crashed`, stops auto-restart, alerts.

### 22.5 Update Flow

Two ways:
- **UI**: click **Redeploy** on app page. Same flow as above, but `git pull` instead of `clone`.
- **Git push trigger**: optional. vekd polls the repo every 60s (cheap) or listens for GitHub webhooks (preferred). On new commit on watched branch, auto-deploys.

### 22.6 Rollback

```bash
vekd apps rollback myblog <release_id>
# or click Rollback in UI
```

Symlinks the `current` release to a previous one, restarts the app.

---

## 23. Process Supervision & cgroups

### 23.1 Why systemd, Not Custom

vekd itself runs as a systemd service. Each app process is a `systemd-run --scope` (or `--unit` for persistence) child. We do not write a custom supervisor in C. Reasons:
- systemd's `Restart=on-failure`, `RestartSec`, `StartLimitBurst` are well-tested.
- `systemctl status vek-myblog` is what every Linux admin already knows.
- `journalctl -u vek-myblog` gives us logs for free.
- cgroup delegation works correctly with systemd.

If systemd is unavailable (e.g., minimal container), vekd falls back to a C-based supervisor using `cgexec` + `waitpid` + a small fork/exec loop. Both paths use the same cgroup config.

### 23.2 cgroup v2

Assume cgroup v2 (modern distros: Ubuntu 22.04+, Debian 12+, Fedora 35+). For v1 cgroups, refuse to start and tell the user to upgrade.

```
/sys/fs/cgroup/vek/
  myblog/
    memory.max        # 512 MB
    memory.high       # soft, 384 MB
    memory.swap.max   # 0 (no swap)
    cpu.max           # "100000 100000" (1 core)
    pids.max          # 256
    cgroup.procs      # PIDs in this cgroup
    cgroup.subtree_control
```

The `cpu.max` format is `$QUOTA $PERIOD` in microseconds. `100000 100000` = 1 full core.

### 23.3 User Isolation

Each app runs as its own system user (`vek_myblog`). No shell, no home dir beyond what vekd creates. The user owns `/var/lib/vek/apps/myblog/` and nothing else. This is the primary security boundary.

If a process escapes its cgroup, it still can't read other apps' data because of filesystem permissions.

### 23.4 No Docker

Deliberate. Docker adds a lot of complexity (image building, layer caching, registry, runtime) for no real benefit on a single-VPS deploy. The image we'd build would be `FROM scratch` + `vek` binary + app `.vebc` anyway, which is just a tarball. cgroups + system users give us the same isolation with fewer moving parts.

If a future user needs Docker, they can `docker run` vekd on a host. The thing vekd supervises is just a process. It doesn't care if that process is a real process or one inside a container vekd itself started.

---

## 24. Cloudflare Integration

### 24.1 Two Modes

**Mode A: Cloudflare in front of vekd (recommended).**
- VPS IP not exposed publicly; only Cloudflare IPs reach vekd.
- Cloudflare handles TLS, DDoS, caching.
- vekd runs on `:80` (HTTP only, behind CF).
- vekd trusts `CF-Connecting-IP` for the real client IP.

**Mode B: Cloudflare Tunnel.**
- vekd spawns `cloudflared` as a child process.
- No inbound ports open on the VPS.
- Tunnel: `myblog.com` → `localhost:80`.
- More secure, slightly more fragile (tunnel can drop).

### 24.2 API Calls

vekd stores a CF API token (encrypted at rest). It uses it to:
- `POST /zones/:id/dns_records` to add A or CNAME.
- `PUT /zones/:id/dns_records/:id` to update.
- `DELETE /zones/:id/dns_records/:id` to remove.
- `POST /zones/:id/purge_cache` on deploy (optional, configurable).

### 24.3 What vekd Does Not Do

- TLS termination in vekd. Use Cloudflare. (v2 may add ACME for direct cert issuance.)
- Page rules / Workers / KV. Use Cloudflare's own products.
- Email routing. Use Cloudflare Email Routing.

---

## 25. Reverse Proxy & Routing

### 25.1 vekd Is the Reverse Proxy

When a request hits vekd:
1. Read first request line: `GET /path HTTP/1.1`.
2. Read `Host` header.
3. Look up app by domain (exact match) OR by `Host` falling back to a default catch-all app.
4. Connect to that app's internal port (kept open, persistent connection pool).
5. Pipe bytes. Add `X-Forwarded-For`, `X-Forwarded-Proto`, `X-Real-IP` if missing.
6. Stream response back.

### 25.2 Routing Table

```sql
select * from apps where domain = :host;
```

If multiple apps claim the same domain, refuse to start. If a domain is set on app A but app B was deployed first, the later wins (warning at deploy time).

### 25.3 Catch-All Domains

If no app matches the host, vekd returns a configurable response:
- A static "no app here" page (default).
- A redirect to a configured `default_app`.

### 25.4 Connection Pool

vekd keeps a pool of N=4 connections per app, reused across requests. On EOF, reconnect. On 5xx from app, try a different connection (allows the app to drain).

### 25.5 Timeouts

- 30s read timeout on app response.
- 60s total request timeout.
- 5s connect timeout to app.

---

## 26. Security Model

### 26.1 Threat Model

The threat is: an untrusted app on the same VPS. Even if the app is malicious (or has a vulnerability), it should not be able to:
- Read other apps' data.
- Crash other apps (it can DoS, but only its own cgroup).
- Exfiltrate the Cloudflare API token.
- Get a shell as root.

### 26.2 Defenses

| Threat                       | Defense |
|------------------------------|---------|
| Read other app's SQLite      | Different system user per app; 0600 on files |
| Read vekd's master key       | 0600 on `/var/lib/vek/master.key`, owned by root, readable only by `vek` group |
| Read Cloudflare token        | Encrypted at rest; decryption needs master key |
| Escape cgroup                | cgroup v2 + no CAP_SYS_ADMIN; modern kernels + `systemd` defaults |
| Privilege escalation         | Apps run as unprivileged system users; no sudo |
| Network sniffing             | Apps bind to `127.0.0.1`; only vekd listens publicly |
| CSRF                         | Mandatory tokens on state-changing requests |
| XSS                          | Auto-escaping in views; `sanitize.html` allowlist |
| SQL injection                | Parameterized queries only; no string concat in `db.query` |
| Path traversal in static     | `public/` files are served by exact path, no `..` |
| Cookie tampering             | HMAC signature; rotation invalidates old |
| Brute-force login            | `ratelimit` is on by default; TOTP 2FA in vkd |
| Secrets in env               | Env file is 0600, owned by app user |

### 26.3 What v1 Does Not Defend Against

- A truly compromised kernel.
- A malicious vekd.
- Side-channel attacks (cache, spectre).
- DoS that exhausts VPS resources (vekd has global rate limits per app but not per IP across apps).

---

## 27. Build Artifact Format (`.vebc`)

See §11.2 for the full layout.

Key points:
- A `.vebc` is a single file, mmap-able.
- It does not contain source code unless `--with-source` is passed.
- It is platform-specific (x86_64-linux, aarch64-linux). Cross-compile in v2.
- It does not contain the `vek` binary itself. vek is installed on the target.
- It is small: a hello-world app is ~30 KB; a real app with vendored markdown is < 1 MB.

### 27.1 Verification

`.vebc` has a SHA-256 of all sections after the header. vekd verifies on load. Corrupted artifacts are rejected and a redeploy is triggered.

### 27.2 Reproducible Builds (v1: best-effort)

`vek build` always:
- Embeds timestamps as 0 (in v1: builds are deterministic given source + compiler version).
- Stores the compiler version in the header.

This means a `vek build` of the same source on the same compiler version produces the same `.vebc` bytes. Useful for caching and for verifying the deployed artifact matches what was approved.

---

## 28. Performance Budget

A reference machine (1 vCPU, 1 GB VPS):

| Operation                          | Target           |
|------------------------------------|------------------|
| Cold start of vek binary           | < 50 ms          |
| Cold start of app (no JIT)         | < 200 ms         |
| HTTP request, no DB, simple render | < 1 ms           |
| HTTP request, 1 DB query, render   | < 5 ms           |
| HTTP request, 3 DB queries, render | < 15 ms          |
| GC pause, 100 MB heap              | < 20 ms          |
| `db.query` of 1000 rows            | < 20 ms          |
| `http.get` to external API         | network-bound    |
| Build of a 5000-line app           | < 5 s            |
| `vek dev` reload of one file       | < 100 ms         |
| `vek fmt` of a 5000-line app       | < 1 s            |
| Memory per app, idle               | < 30 MB          |

These are not SLAs; they're design goals. If the design can't meet them, the design changes, not the goal.

---

## 29. Roadmap

### 29.1 v1 (this design)

- Core language: as specified.
- VM: register, NaN-box, computed goto, mark-sweep.
- 30 stdlib packages.
- SQLite + Postgres.
- File-based routing, views.
- CLI: new, dev, build, run, repl, shell, fmt, check, migrate, test.
- vekd: install, deploy, monitor, route, cgroup, Cloudflare, basic UI.
- Single-user, single-VPS.

### 29.2 v2

- Generational GC.
- Incremental GC.
- Green threads + work stealing.
- HTTP/2 server.
- TLS termination in vekd (ACME).
- Multi-user vekd.
- vek as a library (embeddable in other binaries).
- WebSocket rooms with cross-process broadcast (using Postgres NOTIFY).
- LSP for editor integration.
- Tests: `vek test`, assertions, `assert.eq`, mocks for `db`/`http`.

### 29.3 v3

- Multi-VPS vekd cluster.
- Cross-VPS job queue.
- A built-in pub/sub on top of SQLite/Postgres.
- A "marketplace" of pre-built apps (e.g., a `vek install ghost` flow) — but this is a *content* problem, not a *code* problem; the v1 binary already supports it.

---

## 30. Open Questions

Resolved and open. Resolved ones are kept here as a record of what was decided, so future-me doesn't relitigate.

### Resolved

1. **Numeric tower in v1.** ✅ `1 + 1.0` returns `float`. If any operand is `float`, the result is `float`. **`int / int` always returns `float`** (Python 3 rule, to avoid silent truncation in pagination/averages). For integer division, use `x.div(y)` (returns `int`, floor-rounded) or `x // y` (decided: `div` method, more readable and consistent with `map`/`filter`). Convert int to float explicitly with `x.to_f`. The compiler emits `OP_ADD_INT` / `OP_ADD_FLOAT` accordingly — see §12.2.

2. **Loop-variable closure capture.** ✅ Fresh binding per iteration; closures inside `for` capture the per-iteration local. See §4.16. Matches Python 3+, Ruby, Elixir, modern JS.

3. **`redirect` / `halt` semantics.** ✅ They raise a non-catchable `Unwind` signal that propagates only to the request-handler boundary; `rescue` does not catch it. See §4.15. This eliminates the "stray rescue swallows a redirect" security footgun.

4. **Blocking I/O vs single-threaded event loop.** ✅ Resolved with the **fibers + worker thread pool** model. User code is synchronous-looking; the VM suspends the current fiber, dispatches the I/O to a worker, and resumes when the result is back. See §15.1–15.3.

5. **CSRF vs JSON APIs / webhooks.** ✅ Defaults: `pages/api/` files and webhook routes are CSRF-exempt; HTML pages enforce CSRF. Per-route overrides via `skip_csrf` / `csrf on`. See §17.4.

6. **File uploads.** ✅ `req.files` (and `req.files_iter` for streaming). Configurable size limits. See §7.7.

7. **View DSL string-render perf.** ✅ Internal `Builder` type, separate from user-facing `String`. View helpers write to it; the final string is materialized once. View rendering is O(n), not O(n²). See §8.3.

8. **Global namespace across `pages/`.** ✅ `app.ve` is global; everything else is file-scoped. Put shared helpers in `app.ve`. See §4.12.

9. **File scoping in §4.12 + §5.** ✅ Same as above.

10. **Arithmetic fast-path correctness in §12.2.** ✅ Fixed: the "no tag check" claim only holds for `OP_ADD_INT` / `OP_ADD_FLOAT`, not the generic `OP_ADD`. The type-inference pass is what makes the fast path real.

11. **Symbol literal `:foo` syntax.** ✅ Defined in §4.13: sugar for an interned string, no separate type.

12. **Truthy / falsy rule.** ✅ Only `nil` and `false` are falsy. `0`, `""`, `[]`, `{}` are all truthy. See §4.14.

### Still Open (need answers during implementation)

A. **String interning aggressiveness.** v1 interns identifier-like strings. Should it also intern short string literals? Probably not, but measure. (Cheap to flip later.)

B. **Goroutine vs multi-process for `jobs`.** v1 jobs are in-process worker pool. If you run 4 vek processes for one app, each has its own queue. Cross-process job distribution in v1 is via SQLite polling. Acceptable for ≤ 100 jobs/s. v2 switches to a more efficient protocol (Postgres `LISTEN/NOTIFY` or a small Raft-lite).

C. **Hot reload correctness in dev.** When `app.ve` changes, the in-memory db connection pool, sessions, etc. all need to be reset. v1 just restarts the process on `app.ve` change in dev. v2 may hot-swap.

D. **One binary or many.** Should vekd and vek be one binary with subcommands, or two? **Decision: one binary, two modes.** `vek --server` starts vekd. The reason: install is one thing. vekd is just "vek but in server mode" + a web UI bundle. The web UI is a static directory shipped with the binary.

E. **SQLite lock behavior.** WAL mode handles most cases, but if a long-running read blocks a write, what does `db.exec` do? Block (with a timeout) or fail? **Decision: block for 5 s, then `raise DbTimeoutError`.** Caller can `rescue`.

F. **Per-app database file location.** Default `data/app.db` relative to the app's working directory. vekd sets WorkingDirectory to `/var/lib/vek/apps/<name>/` (NOT inside `current/` — see below) so `data/app.db` lives outside any release. **Decision: `data/` is at the app-dir level, not inside the release.** Releases are pure code + assets; data persists across deploys.

G. **TLS in dev.** None. `http://localhost:3000`. Use `cloudflared tunnel --url http://localhost:3000` if you need HTTPS for testing webhooks.

H. **`Unwind` interaction with future `defer` / cleanup.** v1 has no `defer`; v2 might. The design accommodates it: `defer` blocks run as the fiber unwinds past them, but `Unwind` is a separate signal so v2's `defer` can distinguish a normal return from a `Unwind` (and choose to suppress). Documented for the future; not a v1 problem.

I. **`Unwind` inside `db.transaction do ... end`.** **Decision:** if a closure inside the block Unwinds, the transaction is **rolled back silently** and the request ends. There is no commit, no error, no log line. This is the right behavior — the alternative (commit on redirect) would be a footgun. Documented for completeness.

J. **What does `vek` stand for?** The user named it. It's not an acronym. Don't read into it.

K. **Worker pool queue size cap.** Default 4 × pool size; if exceeded, calling fibers suspend until space frees. Is the multiplier right? Empirically tune during implementation.

---

## 31. Appendix A — Why These Choices

| Choice                                  | Why                                                                       |
|-----------------------------------------|---------------------------------------------------------------------------|
| Ruby/Elixir syntax                      | Readable, agent-friendly, doesn't force OOP, plays well with web code.   |
| Register-based VM                       | Fewer instructions per source construct, more compile-time flexibility.   |
| NaN-boxing                              | Doubles don't pay a tag check; arithmetic is 1 instruction.              |
| Computed GOTO                           | Best-in-class dispatch for x86; portable switch fallback for MSVC.        |
| Mark-and-sweep                          | Simple, robust, no tuning. Generational is a v2 problem.                 |
| Fibers + worker thread pool (v1)        | User code is synchronous-looking; I/O doesn't block other requests. ~200 ns per suspend. |
| Multi-process for multi-core            | No shared state, no thread bugs, easy to debug.                          |
| File-based routing                      | Mental model: "URLs are files." Zero config.                             |
| Single-file pages (handler+UI)          | Leaf/Next inspired. Less context-switching for small features.           |
| 30 stdlib packages, no registry         | v1 can't survive an open ecosystem. Lock the surface, ship a v2 registry if it earns its keep. |
| `app.ve` is the only auto-included file | One obvious place to put config. No `import` in user code.                |
| `app.ve` is the only file with global `fn`s | Prevents silent load-order-dependent name collisions across pages. |
| `redirect` / `halt` are non-catchable (`Unwind`) | A stray `rescue` cannot swallow a redirect. |
| Fresh binding per `for` iteration       | Matches modern languages; eliminates the classic closure-in-loop bug.    |
| Only `nil` and `false` are falsy        | Pythonic, no JS-style coercion trap. `0`, `""`, `[]`, `{}` are truthy.    |
| `int / int` always returns `float`      | Python 3 rule; avoids silent truncation in pagination, averages.          |
| `:foo` is an interned string            | Reads better in `db :sqlite` / `session :cookie`; no new type.            |
| Internal `Builder` for view rendering  | O(n) view rendering, not O(n²). User code still sees `String`.           |
| CSRF-exempt by default for `pages/api/` | JSON APIs use Bearer tokens; CSRF is a cookie-attack defense.             |
| Per-route `skip_csrf` / `csrf on`       | Escape hatches for webhooks, third-party callbacks, and edge cases.      |
| `req.files` + `req.files_iter`          | First-class file uploads with size limits, streaming for large files.     |
| `int` arithmetic needs the typed opcode (`OP_ADD_INT`) to be fast | "No tag check" is a compiler-emission property, not a property of the generic opcode. |
| `data/` lives outside the release dir   | Migrations and SQLite survive deploys.                                   |
| `Unwind` rolls back open `db.transaction`s | A `redirect` in a transaction must not commit. |
| vekd is also a reverse proxy            | One less component. vekd already knows the app topology.                 |
| cgroups, not Docker                     | Docker is overkill for one VPS. cgroups give us 80% of the isolation.      |
| Cloudflare, not nginx                   | Free TLS, free DDoS, free DNS. vekd just calls the API.                  |
| `.vebc` is a single binary              | Easy to ship, easy to verify, easy to cache.                             |
| Two-tier error handling                 | Ruby/Elixir has good taste here. Result for expected, raise for unexpected. |
| No `module` / `import` in v1            | If you have 30 packages and a flat app, you don't need modules yet.      |
| SQLite by default                       | The deploy story writes itself.                                           |
| Systemd for supervision                 | It's already there. Don't reinvent.                                      |

---

## 32. Appendix B — Quick Reference: One App

A complete (tiny) app to show all the moving parts.

### `app.ve`

```ruby
app name: "todo", port: 3000

db :sqlite, path: "data/app.db"

session :cookie,
  secret:    env("SESSION_SECRET"),
  max_age:   7 * 24 * 3600,
  same_site: "lax"

log level: "info", json: env("APP_ENV") == "production"

fn current_user(req)
  uid = req.session["user_id"]
  uid ? db.row("select * from users where id = ?", uid) : nil
end

fn require_login(req)
  u = current_user(req) or redirect "/login"
  u
end

before do |req|
  log.info "req", method: req.method, path: req.path, ip: req.ip
end
```

### `migrations/001_todos.sql`

```sql
create table todos (
  id integer primary key autoincrement,
  user_id integer not null,
  title text not null,
  done integer not null default 0,
  created_at integer not null default (strftime('%s', 'now'))
);
```

### `pages/index.ve`

```ruby
get "/" do
  user = require_login(req)
  todos = db.query(
    "select * from todos where user_id = ? order by id desc",
    user.id
  )
  render "home.ve", user: user, todos: todos
end

post "/" do
  user = require_login(req)
  form.validate(req.form) do
    field "title", required: true, min: 1, max: 200
  end
  if form.valid?
    db.exec(
      "insert into todos (user_id, title) values (?, ?)",
      user.id, form["title"]
    )
  end
  redirect "/"
end

post "/:id/toggle" do
  user = require_login(req)
  db.exec(
    "update todos set done = 1 - done where id = ? and user_id = ?",
    req.params["id"], user.id
  )
  redirect "/"
end
```

### `views/layouts/main.ve`

```ruby
fn call(title, &content)
  doctype(:html5)
  html(lang: "en") do
    head do
      title "Todo — #{title}"
      link(rel: "stylesheet", href: "/style.css")
    end
    body do
      h1 "Todo for #{@user.email}"
      content()
    end
  end
end
```

### `views/home.ve`

```ruby
layout "main.ve", title: "Home" do
  form(method: "post", action: "/") do
    input(name: "title", placeholder: "What needs doing?", autofocus: true)
    button "Add"
  end

  ul.todos
    for t in @todos
      li(class: t.done ? "done" : nil) do
        form(method: "post", action: "/#{t.id}/toggle", style: "display:inline") do
          button(if t.done then "✓" else "○" end)
        end
        span t.title
      end
    end
end
```

That's the whole app. `vek dev` to run it. `vek build` to produce a `.vebc`. `vekd` to deploy.

---

## End of Document

If you read this far, you know more about vek than I will after a month of not looking at it. The decisions here are the spec. When implementation starts, deviations get a new version number or a new section in §30, not silent drift.
