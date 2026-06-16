# Order Matching Engine

A C++ order matching engine with a FastAPI/WebSocket bridge and a React frontend showing live candlestick charts and Level 2 order book data.

## Architecture

Three Docker containers, one entry point:

```
Browser → nginx :80 ─┬─ /            → React SPA (static files)
                     ├─ /ws/chart    → FastAPI (WebSocket)
                     └─ /get-level2-data → FastAPI (REST)

FastAPI → gRPC :50051 → C++ matching engine
```

| Container | What it runs |
|-----------|-------------|
| `cpp-server` | C++ gRPC matching engine (port 50051, internal only) |
| `backend` | FastAPI + WebSocket bridge to the engine (port 8000, internal only) |
| `frontend` | nginx serving the React build + reverse proxy to backend |

Only port 80 is exposed to the host.

## Running the app

**Requirements:** Docker with Compose.

```bash
docker compose up -d
```

Then open [http://localhost](http://localhost).

The chart will be empty until a client starts seeding orders (see below).

To stop:

```bash
docker compose down
```

## Running a client

The `mean_reversion_client` is built into the `cpp-server` image. It simulates two-sided market making — a bid thread and an ask thread that watch each other and place orders to keep the spread tight and move the price up/down on a cycle.

Run it against the live server:

```bash
docker exec -it order-matching-engine-cpp-server-1 ./mean_reversion_client
```

With options (all optional):

```bash
docker exec -it order-matching-engine-cpp-server-1 \
  ./mean_reversion_client -s 500.0 -r 60 -p 50 -l 0.25
```

| Flag | Default | Meaning |
|------|---------|---------|
| `-s` | 500 | Seed price ($) |
| `-o` | 500 | Average order size |
| `-p` | 100 | Milliseconds between orders |
| `-r` | 180 | How long to run (seconds) |
| `-a` | 0.2 | Aggressiveness factor |
| `-l` | 0.50 | Max spread before tightening ($) |

Once the client is running, the candlestick chart and Level 2 table at [http://localhost](http://localhost) will update in real time.

## Deploying to a different host

The React bundle bakes in `http://localhost` at build time. To deploy to a domain, rebuild with:

```bash
docker compose build \
  --build-arg REACT_APP_API_HTTP_URL=https://yourdomain.com \
  --build-arg REACT_APP_API_WS_URL=wss://yourdomain.com \
  frontend
docker compose up -d
```
