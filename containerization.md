# Containerization Plan

## Goal

Make the full stack runnable with a single `docker compose up` command on any machine,
with no host-side dependencies beyond Docker. The result should be linkable from a
personal site as a live demo.

---

## Current Blockers (Must Fix Before Containers Work)

These are code-level problems that will break any container build. They must be
resolved as part of the containerization work, in the order listed.

### 1. `frontend/package.json` is missing

`package.json` was never committed (it was likely inside the gitignored `node_modules`
parent or lost when `build/` was excluded). Without it, the React app cannot be
installed or built. It must be recreated.

Known dependencies from `App.js`:
- `react`, `react-dom`, `react-scripts` (Create React App)
- `react-plotly.js` (the `Plot` import)
- `plotly.js` (peer dep of react-plotly.js)

A `package-lock.json` should also be committed once created so Docker builds are
reproducible.

### 2. Hardcoded `protoc` paths in `CMakeLists.txt`

`CMakeLists.txt:4-5` hardcodes:
```
set(PROTOC_PATH "/Users/mananmanchanda/.local/bin/protoc")
set(GRPC_CPP_PLUGIN_PATH "/Users/mananmanchanda/.local/bin/grpc_cpp_plugin")
```

These paths do not exist in any container. They must be replaced with
`find_program(PROTOC_PATH protoc REQUIRED)` and
`find_program(GRPC_CPP_PLUGIN_PATH grpc_cpp_plugin REQUIRED)`, so CMake resolves
the tools from the container's `PATH`.

### 3. Hardcoded `localhost` URIs in the Python backend

`backend/app.py:50` and `backend/live_candlestick.py:47` both hardcode
`localhost:50051`. Inside Docker, the C++ server runs in a separate container
and is only reachable by service name (e.g., `cpp-server:50051`).

Replace with: `os.environ.get("GRPC_SERVER_HOST", "localhost") + ":50051"`.
No other logic changes are needed — just the channel target string.

### 4. Hardcoded `localhost:8000` and `localhost:8000` in the React frontend

`frontend/src/App.js:17` uses `http://localhost:8000/get-level2-data` and
`:23` uses `ws://localhost:8000/ws/chart`. When served by nginx in a container,
the browser talks directly to those URLs — but in a deployment, `localhost` is the
user's machine, not the container, so this actually works in simple dev setups but
breaks in hosted/cloud deployments.

Replace with `REACT_APP_API_HTTP_URL` and `REACT_APP_API_WS_URL` environment
variables (CRA's `REACT_APP_` prefix bakes env vars into the static build at
`npm run build` time). Provide sensible defaults for local use. This makes the
frontend portable to any hostname without a rebuild.

### 5. `backend/requirements.txt` is missing

The Python backend has no dependency manifest. One must be created listing:
- `fastapi`
- `uvicorn[standard]`
- `grpcio`
- `grpcio-tools` (needed to generate Python stubs at build time)
- `protobuf`

Pin major versions to ensure the container build is reproducible.

---

## Service Architecture

Four services, one Docker Compose file at the repo root.

```
┌──────────────────────────────────────────────────────────────────────┐
│  Browser (user)                                                      │
│    HTTP/WS  ──→  :80 (nginx/frontend)                               │
│    nginx reverse proxy  ──→  :8000 (python-backend)                 │
└──────────────────────────────────────────────────────────────────────┘
                                    │
                         ┌──────────▼──────────┐
                         │  python-backend :8000│
                         │  FastAPI + WebSocket │
                         └──────────┬──────────┘
                                    │ gRPC :50051
                    ┌───────────────▼────────────────┐
                    │         cpp-server :50051       │
                    │   C++ Matching Engine (gRPC)    │
                    └───────────────┬────────────────┘
                                    │ gRPC :50051
                    ┌───────────────▼────────────────┐
                    │  mean-reversion-client          │
                    │  (optional demo driver)         │
                    └────────────────────────────────┘
```

### Service: `cpp-server`
- Builds the C++ matching engine server binary
- Exposes port 50051 (gRPC, internal only — not exposed to host)
- Requires gRPC C++ and protobuf installed in the build environment
- Uses a multi-stage Dockerfile (see below)

### Service: `python-backend`
- Generates Python gRPC stubs from `matching_engine.proto` at build time
- Runs FastAPI via `uvicorn`
- Exposes port 8000 (internal only)
- Depends on `cpp-server` being healthy before starting

### Service: `frontend`
- Builds the React app with `npm run build`
- Serves the static output via nginx
- Exposes port 80 (the only port exposed to the host)
- nginx also reverse-proxies `/api/` and `/ws/` to `python-backend:8000`
  so the browser only needs to talk to one origin (eliminates CORS entirely)
- Bakes `REACT_APP_*` env vars at build time

### Service: `mean-reversion-client`
- Built in the same multi-stage Dockerfile as `cpp-server`
- Runs the mean-reversion client binary against `cpp-server:50051`
- Depends on `cpp-server` being healthy
- This is what populates the order book so the chart shows real activity
- Should be restartable in case it exits before the server
- Optional: can be skipped (`docker compose up --scale mean-reversion-client=0`)
  for a "bare" server where you drive orders manually

---

## Dockerfile Strategy

### `Dockerfile.cpp` (for both `cpp-server` and `mean-reversion-client`)

**Why a separate file:** The C++ build is expensive (~5–10 min first time).
A dedicated file with careful layer ordering ensures the dependency install
layer is cached even when source code changes.

**Stage 1 — builder:**
- Base: `debian:bookworm-slim` (or `ubuntu:22.04`)
- Install build tools: `cmake`, `make`, `g++`, `pkg-config`
- Install gRPC C++ and protobuf: via `apt` if the distro packages are
  recent enough (Debian Bookworm has grpc 1.51), or via `vcpkg`/manual build
  if not. Debian Bookworm's `libgrpc++-dev` and `protobuf-compiler-grpc`
  packages are sufficient and avoid a from-source grpc build (~45 min).
- Copy `matching_engine.proto` and `src/` into the build context
- Copy the fixed `CMakeLists.txt`
- Run `cmake` then `cmake --build`
- Output: `build/matching_engine_server`, `build/mean_reversion_client`

**Stage 2 — runtime:**
- Base: `debian:bookworm-slim`
- Install only the runtime shared libraries (`libgrpc++`, `libprotobuf`,
  `libstdc++`) — not the dev headers or build tools
- Copy binaries from Stage 1
- Exposes port 50051
- Default `CMD`: `./matching_engine_server`

The `mean-reversion-client` service uses the same image but overrides `CMD`
to `./mean_reversion_client`.

**Why not Alpine:** gRPC C++ uses glibc; musl libc (Alpine) requires extra
patching. Debian slim is ~80MB and avoids that complexity entirely.

**Estimated final image size:** ~120MB (slim base + grpc runtime libs + binary)

### `Dockerfile.backend`

**Stage 1 — builder (proto generation):**
- Base: `python:3.11-slim`
- Install `grpcio-tools`
- Copy `matching_engine.proto`
- Run `python -m grpc_tools.protoc` to generate `matching_engine_pb2.py`
  and `matching_engine_pb2_grpc.py`

**Stage 2 — runtime:**
- Base: `python:3.11-slim`
- Copy `requirements.txt`, run `pip install --no-cache-dir -r requirements.txt`
- Copy `backend/` source files
- Copy generated `*_pb2.py` files from Stage 1 into the backend directory
- Set `GRPC_SERVER_HOST` env var default to `cpp-server`
- CMD: `uvicorn app:app --host 0.0.0.0 --port 8000`

**Why separate proto generation stage:** Keeps the installed `grpcio-tools`
(compile-only dep, ~150MB) out of the final runtime image.

### `Dockerfile.frontend`

**Stage 1 — node builder:**
- Base: `node:20-slim`
- Copy `frontend/package.json` and `frontend/package-lock.json` first
- Run `npm ci` (installs exact locked versions, faster and more reproducible
  than `npm install`)
- Copy rest of `frontend/src/` and `frontend/public/`
- Set `REACT_APP_API_HTTP_URL=/api` and `REACT_APP_API_WS_URL=/ws`
  (relative paths that go through nginx reverse proxy on same origin)
- Run `npm run build`

**Stage 2 — nginx:**
- Base: `nginx:alpine` (~10MB)
- Copy built static files from Stage 1 into `/usr/share/nginx/html/`
- Include a custom `nginx.conf` that:
  - Serves `/` from static files
  - Reverse-proxies `/api/` to `python-backend:8000`
  - Reverse-proxies `/ws/` to `python-backend:8000` (WebSocket upgrade headers required)
- Exposes port 80

**Estimated final image size:** ~15MB

---

## Docker Compose File Structure

File: `docker-compose.yml` at repo root.

Key behaviors:
- `cpp-server` has a health check that uses `nc` (netcat) to test `:50051`
  is open (or `grpc_health_probe` if we include it in the image)
- `python-backend` has `depends_on: cpp-server: condition: service_healthy`
- `mean-reversion-client` has `depends_on: cpp-server: condition: service_healthy`
  and `restart: on-failure` (it exits after `RUN_TIME` seconds; restart keeps the demo alive)
- `frontend` has no service dependencies (nginx serves static files; API calls
  fail gracefully until backend is up)
- Only `frontend:80` is exposed to the host; all other ports are internal

Environment variables exposed at the compose level:
- `GRPC_SERVER_HOST` (default: `cpp-server`)
- `SEED_PRICE` (default: `500`, passed to mean-reversion-client via CLI args)
- `RUN_TIME` (default: `180`, how long each mean-reversion client run lasts)

---

## nginx Configuration Details

The nginx config is the key piece that makes the frontend portable. By proxying
all API calls through nginx, the browser only ever talks to one origin (port 80),
which eliminates the hardcoded `localhost:8000` entirely and removes CORS concerns.

Required directives:
- Standard HTTP proxy to `python-backend:8000` for `/api/` routes
- WebSocket proxy for `/ws/chart` — requires `Upgrade` and `Connection` headers
  to be forwarded, and `proxy_read_timeout` to be set high (WebSocket is long-lived)
- `try_files $uri /index.html` for client-side routing

---

## Build Context and `.dockerignore`

Each Dockerfile needs a `.dockerignore` (or use compose's `context` + `dockerfile`
fields to limit what gets sent to the daemon):

**C++ build context:** Should include only `src/`, `CMakeLists.txt`,
`matching_engine.proto`. Exclude `build/`, `frontend/`, `backend/`, `tests/`.

**Python build context:** Should include only `backend/`, `matching_engine.proto`.

**Frontend build context:** Should include only `frontend/`. Exclude
`frontend/node_modules/` (will be rebuilt in container).

Using separate contexts prevents sending the entire monorepo to the daemon
for each service, which would otherwise be slow and wasteful.

---

## Startup Order and Timing

Expected startup sequence and approximate times (first run, cold cache):

```
t=0s    cpp-server starts building (apt installs, cmake, compile ~5-10 min first time)
        After that, cached. Incremental rebuilds ~30s for source-only changes.

t=0s    frontend, python-backend build in parallel with cpp-server

t=?     cpp-server container starts, exposes :50051
        Health check polls :50051 every 5s, up to 30s timeout

t=?+5s  python-backend starts (depends_on healthy cpp-server)
        mean-reversion-client starts (same condition)

t=?+5s  mean-reversion-client begins populating order book with AAPL orders
        (uses default SEED_PRICE=500, pushes bids and asks, market starts moving)

t=?+5s  python-backend's startup event fires: subscribe_to_price("AAPL") begins
        streaming from cpp-server; candlestick chart starts accumulating data

t=?+10s WebSocket connections from browser begin receiving candle updates
```

The `mean-reversion-client` runs for `RUN_TIME` seconds (default 180s) then exits.
With `restart: on-failure` in compose, it restarts automatically, keeping the demo
market active indefinitely.

---

## File Changes Summary

| File | Change |
|------|--------|
| `CMakeLists.txt` | Replace hardcoded protoc/plugin paths with `find_program()` |
| `backend/app.py` | Replace `localhost:50051` with `os.environ.get("GRPC_SERVER_HOST", "localhost") + ":50051"` |
| `backend/live_candlestick.py` | Same change as above |
| `backend/requirements.txt` | Create (does not exist) |
| `frontend/package.json` | Create (was never committed) |
| `frontend/package-lock.json` | Create (run `npm install` locally first to generate) |
| `frontend/src/App.js` | Replace hardcoded URLs with `process.env.REACT_APP_*` |
| `docker-compose.yml` | Create |
| `Dockerfile.cpp` | Create |
| `Dockerfile.backend` | Create |
| `Dockerfile.frontend` | Create |
| `nginx.conf` | Create (for frontend service) |
| `.dockerignore` | Create (one per build context, or a single root-level one) |

**No changes to core C++ source files (`src/`), the proto file, or test files.**

---

## Risks and Mitigations

| Risk | Likelihood | Mitigation |
|------|-----------|-----------|
| Debian Bookworm `libgrpc++-dev` version incompatible with proto generated by newer `protoc` | Medium | Pin protoc and grpc versions; generate proto inside the same container |
| C++ build layer cache misses on every `docker compose build` | High without care | Order `COPY CMakeLists.txt` and dependency install before `COPY src/`; source changes only invalidate the compile step |
| `mean-reversion-client` exits and `restart: on-failure` causes rapid restart loop if cpp-server is slow | Low | Add a startup sleep (`--wait 5`) or use `restart: unless-stopped` with a sleep in the entrypoint |
| WebSocket proxy timeout through nginx cutting long-lived connections | Medium | Set `proxy_read_timeout 3600s` in nginx config |
| gRPC proto version skew between C++ server (compiled with one protobuf) and Python client (installed separately) | Medium | Generate Python stubs inside `Dockerfile.backend` using the same `matching_engine.proto`; pin `grpcio` and `protobuf` versions in `requirements.txt` to match what Debian packages |
| `frontend/package.json` reconstruction is incomplete (missing a dep) | Low | Run `npm start` locally after recreation and fix any import errors before committing |

---

## Testing the Containerized Stack

After `docker compose up`:

1. Open `http://localhost` in a browser
2. Wait ~10s for the mean-reversion-client to populate the order book
3. Candlestick chart should begin showing candles (1-second candle size by default)
4. Level 2 table should show bids (green) and asks (red) updating every 1s
5. To confirm the gRPC layer is working: `docker compose logs python-backend`
   should show no gRPC connection errors
6. To confirm the matching engine is active: `docker compose logs cpp-server`
   should show "Matching order" lines as trades execute

---

## Implementation Order

Execute these steps in sequence to avoid unblocking issues:

1. Fix `CMakeLists.txt` (protoc paths) — needed for C++ Dockerfile to work at all
2. Recreate `frontend/package.json` + `package-lock.json` locally (`npm install`)
3. Create `backend/requirements.txt`
4. Fix `backend/app.py` and `live_candlestick.py` (localhost → env var)
5. Fix `frontend/src/App.js` (localhost URLs → REACT_APP_ env vars)
6. Write `Dockerfile.cpp` and verify `docker build` produces a working binary
7. Write `Dockerfile.backend` and verify the Python stubs generate correctly
8. Write `Dockerfile.frontend` + `nginx.conf` and verify static build + proxy
9. Write `docker-compose.yml` and run `docker compose up`
10. Smoke test (browser to `http://localhost`, verify chart and Level 2 table)
