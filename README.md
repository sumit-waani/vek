# vek

A web-first programming language with a built-in HTTP server, file-based routing,
and batteries-included standard library. Compiles to register-based bytecode and
runs on a custom VM with NaN-boxed values and a fiber-based concurrency model.

---

## Getting Started

### Prerequisites

- A [Turso](https://turso.tech) database (provides the `TURSO_DATABASE_URL` and `TURSO_AUTH_TOKEN`)
- An S3-compatible object storage bucket (provides `S3_ENDPOINT`, `S3_ACCESS_KEY`, `S3_SECRET_KEY`, `S3_BUCKET`)
- Optionally: a Redis instance for distributed `kv`/`cache` (provides `REDIS_URL`)

### Create a New Project

```sh
vek new myapp
cd myapp
```

### Configure Environment

Fill in the generated `.env` file with your credentials:

```env
TURSO_DATABASE_URL=libsql://your-db-name.turso.io
TURSO_AUTH_TOKEN=your-token-here
S3_ENDPOINT=https://your-s3-endpoint.com
S3_ACCESS_KEY=your-access-key
S3_SECRET_KEY=your-secret-key
S3_BUCKET=your-bucket-name
SECRET_KEY_BASE=generate-a-random-64-char-string
# Optional: enable distributed kv/cache
# REDIS_URL=redis://localhost:6379
```

> **Note:** A Turso database is required. Create one at https://turso.tech before
> running your app. The `vek` CLI checks that all required env vars are present
> and fails fast with a clear error if any are missing.

### Run in Development

```sh
vek dev
```

This starts the dev server with hot reload. File changes are detected and
recompiled automatically.

---

## Project Structure

```
myapp/
  app.ve           # Application entry point
  routes/
    index.ve       # GET /
    about.ve       # GET /about
    posts/
      index.ve     # GET /posts
      [id].ve      # GET /posts/:id
  views/
    layout.ve      # Layout template
  migrations/
    001_create_posts.sql
  public/
    style.css
  .env             # Environment variables (not committed)
  Dockerfile       # Generated, ready for deployment
  fly.toml         # Generated, ready for Fly.io
```

---

## CLI Commands

| Command | Description |
|---|---|
| `vek new <name>` | Scaffold a new project |
| `vek dev` | Start dev server with hot reload |
| `vek build` | Compile to `.vebc` artifact |
| `vek run` | Run a compiled `.vebc` artifact |
| `vek fmt` | Format `.ve` source files |
| `vek repl` | Interactive language REPL |
| `vek shell` | REPL with app context (db, kv, etc.) |
| `vek migrate` | Run pending database migrations |
| `vek test` | Run test suite |

---

## Deployment

Deployment uses standard Docker tooling. The `vek new` command generates a
`Dockerfile` and `fly.toml` ready for use.

### Build and Deploy

```sh
# Build the app
vek build

# Build Docker image
docker build -t myapp .

# Deploy to Fly.io
flyctl deploy
```

### Environment Variables

Set these on your deployment platform (Fly.io secrets, Docker env, etc.):

| Variable | Required | Description |
|---|---|---|
| `TURSO_DATABASE_URL` | Yes | Turso database URL |
| `TURSO_AUTH_TOKEN` | Yes | Turso authentication token |
| `S3_ENDPOINT` | Yes | S3-compatible storage endpoint |
| `S3_ACCESS_KEY` | Yes | S3 access key ID |
| `S3_SECRET_KEY` | Yes | S3 secret access key |
| `S3_BUCKET` | Yes | S3 bucket name |
| `SECRET_KEY_BASE` | Yes | Secret key for session signing |
| `REDIS_URL` | No | Redis URL for distributed kv/cache |
| `PORT` | No | HTTP port (default: 8080) |

The `vek` CLI never shells out to `turso`, `fly`, `aws`, or any other vendor CLI.
You provision your own resources and supply credentials via environment variables.

---

## Standard Library

vek ships with 30 built-in packages:

| Package | Description |
|---|---|
| `db` | Turso/libsql database (queries, transactions, connection pool) |
| `kv` | In-memory key-value store (LRU with TTL); optional Redis backend |
| `cache` | TTL cache with `get_or_set`; optional Redis backend |
| `json` | JSON encode/decode |
| `form` | Form validation DSL |
| `session` | Signed cookie sessions |
| `csrf` | CSRF token generation/validation |
| `auth` | Bcrypt hash/verify, random tokens |
| `log` | Structured logging (text/JSON modes) |
| `env` | Env var access with defaults |
| `time` | Time operations and formatting |
| `uuid` | UUID v4 and v7 generation |
| `crypto` | SHA-256, HMAC, random bytes |
| `path` | URL path utilities |
| `http` | HTTP client with timeouts and retries |
| `mail` | SMTP email sending |
| `jobs` | Background job queue (persisted to Turso, retry with backoff) |
| `storage` | S3-compatible blob storage |
| `flash` | One-shot session messages |
| `ratelimit` | Token bucket rate limiting |
| `compress` | Gzip and Brotli compression |
| `websocket` | WebSocket connections |
| `i18n` | Key-based translations |
| `webhook` | Webhook signature verification |
| `markdown` | Markdown to HTML |
| `sanitize` | HTML sanitization (allowlist) |
| `csp` | Content-Security-Policy builder |
| `slug` | URL slugification |
| `cors` | CORS header management |
| `cli` | stdin, args, ANSI colors |

---

## Architecture Highlights

- **Register-based VM** with NaN-boxed 64-bit values
- **Computed GOTO dispatch** (GCC/Clang) for near-1-op-per-cycle throughput
- **Fiber-based concurrency** with epoll/kqueue event loop
- **Mark-and-sweep GC** with 16 KB page-based heap
- **File-based routing** with dynamic segments and constraints
- **Single binary** containing compiler, VM, HTTP server, and all stdlib packages

---

## Building from Source

```sh
# Requires clang (C11) on Linux x86_64
make
make test
```

---

## License

See LICENSE file.
