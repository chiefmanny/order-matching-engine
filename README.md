# Concurrent C++ Matching Engine

[![CI](https://github.com/chiefmanny/order-matching-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/chiefmanny/order-matching-engine/actions/workflows/ci.yml)

A multithreaded limit order book in C++17, with price-time priority, per-symbol books, and a gRPC server that streams market data to multiple clients. I wrote the original engine by hand in 2024 while exploring C++ concurrency and the STL, then later built a market simulation on top of it.

- **The main finding:** finer-grained locking gave no measurable speedup in my benchmark and introduced a use-after-free that crashed the server under load.
- **See it run:** [recorded sessions replay in the browser](https://manan-opal.vercel.app/projects/market-microstructure). The engine matched every order, and [finchart](https://github.com/chiefmanny/finchart) draws the charts.

## Who wrote what

| Work | When | How it was written |
|---|---|---|
| [Engine, gRPC server, and original tests](https://github.com/chiefmanny/order-matching-engine/tree/handwritten-2024) | 2024 | By hand, every line |
| [Fine-grained locking branch](https://github.com/chiefmanny/order-matching-engine/tree/fine-grained-2024) | 2024 | By hand |
| Engine fixes, simulated traders, experiment harness | 2026 | AI-assisted |
| Benchmark setup and this README | 2026 | AI-assisted |
| [finchart](https://github.com/chiefmanny/finchart), the Rust charting library | 2026 | AI-assisted |

Of the 779 lines in `src/core` today, 438 date from 2024 and 341 from 2026. To read only my hand-written code, use the [`handwritten-2024`](https://github.com/chiefmanny/order-matching-engine/tree/handwritten-2024) tag.

## How the engine works

```
 clients <──gRPC──> server thread pool ──add / cancel / read──> OrderBook (one per symbol)
                            ^                                     asks: std::map<price, level>
                            │ fills                               bids: std::map<price, level>
                            │                                     level: FIFO deque of orders
                            │                                     index: order id -> order
                            │                                             ^
                     matching thread (one per symbol) ──crosses the book──┘
```

- **Matching:** each symbol has its own matching thread. It wakes on a 1 ms timer, locks both sides of the book, and crosses the best bid and ask until they no longer overlap. Orders at a price fill first-in, first-out.
- **Locking:** each side of the book has a `std::shared_mutex`, and so does each order index. The matcher takes both sides with [`std::lock`](https://github.com/chiefmanny/order-matching-engine/blob/handwritten-2024/matching_algorithm.cpp#L34), and writers use `std::scoped_lock`, which avoids deadlock from acquiring those locks in different orders.
- **API:** `AddLimitOrder`, `CancelLimitOrder`, and `CheckSymbol` are single calls. `WatchSymbol`, `Subscribe`, and `GetLevel2` stream quotes, fills, and depth.

```
src/core/            order book + matching algorithm
src/grpc/            gRPC server and client
src/clients/         benchmarks, participant framework, trading strategies
tests/unit/          C++ tests
experiments/         scenario runner, recorder, metrics, plots, presets
backend/ frontend/   FastAPI + React demo showing live candles and Level 2
docs/benchmarks/     benchmark setup and raw results
```

## The locking experiment

The 2024 engine locks a whole side of the book for every operation, so adding an order at one price blocks an add at any other. I built a [fine-grained version](https://github.com/chiefmanny/order-matching-engine/tree/fine-grained-2024) with [a mutex per price level](https://github.com/chiefmanny/order-matching-engine/blob/fine-grained-2024/limit_price.h#L31), so a writer only holds the tree lock long enough to find its level. `master` stayed on coarse locks.

In November 2026 I benchmarked all three versions side by side, with the same compiler, flags, and client programs:

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/benchmarks/results-dark.png">
  <img src="docs/benchmarks/results.png" alt="Two charts. Left: median test duration against orders per client on log axes, where the coarse, fine-grained, and current engines overlap almost exactly. Right: fine-grained server aborts out of 10 runs at each size: 1 at 12,800 orders, 1 at 25,600, 6 at 51,200, and 9 at 102,400. The fine-grained server aborted in 17 of 110 runs, and coarse and current in 0 of 220.">
</picture>

| Orders per client | Coarse | Fine-grained | Current | Fine-grained runs aborted |
|---:|---:|---:|---:|---:|
| 1,600 | 1.18 s | 1.17 s | 1.17 s | 0 of 10 |
| 12,800 | 2.40 s | 2.40 s | 2.39 s | 1 of 10 |
| 51,200 | 7.99 s | 8.01 s | 8.07 s | 6 of 10 |
| 102,400 | 15.44 s | 15.45 s (1 clean run) | 15.57 s | 9 of 10 |

Median single-order acknowledgement latency was 96-97 µs for all three, and the largest size works out to about 13,000 order submissions per second across two clients. These are gRPC round trips measured in a Linux VM, so they say nothing about how fast the matching itself is. See the [method, caveats, and raw data](docs/benchmarks/README.md).

### Why it crashed

[`addLimitOrder`](https://github.com/chiefmanny/order-matching-engine/blob/fine-grained-2024/order_book.cpp#L28-L48) finds the price level, releases the tree lock, and only then locks the level. Simplified:

```cpp
{
    std::shared_lock<std::shared_mutex> sharedLock(bidMutex);
    limitPrice = &it->second;                  // pointer into the map
}                                              // tree lock released here
                                               // <-- the matcher can erase the level here
std::lock_guard<std::mutex> lock(limitPrice->mtx);
limitPrice->orders.push_back(addOrder);        // use-after-free
```

If the matcher [empties and erases that level](https://github.com/chiefmanny/order-matching-engine/blob/fine-grained-2024/matching_algorithm.cpp#L111-L115) inside the gap, the writer uses freed memory. AddressSanitizer caught it in 3 of 3 runs, with [the free and the bad access in those exact places](docs/benchmarks/results/asan/fine_report.txt). Coarse and current ran clean under the same load.

The obvious fix, holding the tree lock while taking the level lock, would deadlock: the matcher holds a level lock while waiting for the exclusive tree lock, which is the opposite order. A correct version needs reference-counted levels and a "retired" flag that writers re-check after locking, retrying if the level is gone. I haven't built that, because the results below say the locks aren't where the time goes.

This bug also nearly fooled me. Failed RPCs return instantly, so an early benchmark run showed the fine-grained version finishing almost three times faster. The speedup was the server crashing. The benchmark now records whether the server survived every run.

### Why it wasn't faster

Times were within about 1% at the largest size, so whatever limits this test isn't the book's locks. The likeliest cause is the gRPC round trip, at nearly 100 µs per order, though I haven't timed the book in isolation to confirm it. The test also has only two clients and one matcher, which is light contention, so fine-grained locking might still help with many more writers.

### Earlier debugging

The 2024 history shows the rest of that process:

- A [bad access from `.front()` on a level's orders](https://github.com/chiefmanny/order-matching-engine/commit/b81ca70), tracked down with LLDB
- [Volume counted twice](https://github.com/chiefmanny/order-matching-engine/commit/385fbf5) on full fills
- A [cancel that erased a bid level from the ask tree](https://github.com/chiefmanny/order-matching-engine/commit/4a0826b) in the fine-grained branch
- A [test showing](https://github.com/chiefmanny/order-matching-engine/commit/7f378a3) that one client's requests run in order, while requests from different clients can interleave

I used LLDB, [AddressSanitizer](https://github.com/chiefmanny/order-matching-engine/commit/05214b2), and Valgrind, which meant moving the project into an Ubuntu VM.

## Bugs fixed since 2024

My hand-written engine had real bugs. These were found and fixed during the AI-assisted 2026 work on the market simulation. They're all in [`d17cc8b`](https://github.com/chiefmanny/order-matching-engine/commit/d17cc8b), and `book_invariant_tests` now covers them.

| Bug | Effect | Fix |
|---|---|---|
| [Trades printed at the midpoint](https://github.com/chiefmanny/order-matching-engine/blob/handwritten-2024/matching_algorithm.cpp#L61) of the two limit prices | Wrong trade prices, and slippage measurements were an artifact | Orders carry an arrival sequence, and the resting order sets the price |
| `LimitPrice::totalSize` never initialized or reduced on fills | Level 2 depth numbers were undefined and drifted | Initialized, and maintained on every fill |
| Order ids drawn from the full unsigned range, but sent as `int32` | Ids above 2^31 came back negative | Drawn from `[1, INT32_MAX]` |
| Filled orders stayed in the order index | Cancelling a filled order could succeed and corrupt depth | Filled ids are removed after each match |

The November 2026 benchmarking pass also fixed a March 2024 bug in the stress tests: a buy ratio above 100% made an unsigned count wrap around, so the test tried to add about 4.3 billion orders and could never finish.

## What I'd change for low latency

This engine is built around shared-state concurrency, not low latency, and it shows:

| Today | Cost | What I'd build instead |
|---|---|---|
| Matcher [sleeps 1 ms between passes](src/core/matching_algorithm.cpp#L209) | A crossing order can wait about a millisecond to match | Match on arrival, inside the order's own processing |
| Many threads share the book behind locks | Contention and unpredictable waits | One writer thread per symbol, fed by a queue |
| `std::map<double, ...>` for price levels | Pointer chasing, cache misses, and floating-point keys | Integer tick prices with array-indexed levels |
| A `shared_ptr` allocation per order | An allocator call and atomic ref-counting on the hot path | Pre-allocated pool with intrusive lists |
| gRPC on every order | Nearly 100 µs per round trip in this setup | A binary protocol, with the core measured in process |

A hand-written core along those lines, benchmarked in process and reporting latency percentiles, is the next project.

## Build and test

The core and fast tests need only CMake and a C++17 compiler. nlohmann/json comes from your system if installed, otherwise from GitHub at configure time.

```bash
cmake -B build && cmake --build build -j8
./build/participant_tests        # strategies, order tracking, tick math, event log
./build/book_invariant_tests     # depth bookkeeping and the trade tape
./build/order_book_tests         # multithreaded stress tests, about 15 seconds
python3 experiments/tests/test_scenario.py
python3 experiments/tests/test_metrics.py
```

The server and clients also build when gRPC and protobuf are installed. The easiest route is Docker:

```bash
docker build -f Dockerfile.cpp -t ome-cpp:latest .
```

## Market simulation

Simulated traders run against the real engine over gRPC, and Python only orchestrates and analyzes. The harness states a hypothesis before each run and reports whether it held. This part is AI-assisted. It's here because it's where the engine's bugs surfaced, and because it produces the recordings in the browser demo.

### Market participants

One executable, `participant_runner`, becomes whichever participant its config asks
for. Adding a participant is one strategy file plus one line in the registry — never a
new binary.

| strategy | what it does |
|---|---|
| `liquidity_provider` | maintains a multi-level two-sided book; replenishes, re-centres, and shifts its quotes as inventory accumulates. Supports `mode: withdraw` to pull its book mid-run |
| `noise_trader` | background order flow, both sides, partly aggressive, with cancellations |
| `market_maker` | quotes near the touch, skews on inventory, withdraws a side at its limit |
| `directional_trader` | sustained one-sided pressure over a configurable duration |
| `parent_executor` | works a large parent order: `immediate`, `sliced`, `participation`, or `passive_first` |
| `sweep` | alias for `parent_executor` in immediate mode |
| `layering_demo` | educational manipulation-detection demonstration (see below) |

Strategies depend only on a narrow `OrderGateway` interface, so they are unit-tested
against a fake gateway with no engine, no gRPC and no threads.

### Running an experiment

```bash
pip install -r experiments/requirements.txt
docker build -f Dockerfile.cpp -t ome-cpp:latest .

experiments/run.py experiments/scenarios/static_book.yaml
experiments/analyze.py experiments/results/static_book/run_001
```

`run.py` starts the engine, attaches a recorder, launches participants on a timeline,
stops everything cleanly, and judges the run. Each run produces a self-contained
directory:

```
trades.csv  bbo.csv  level2.csv  candles_*.csv
participant_events.jsonl   summary.json   metadata.json   plots/  logs/
```

A run is only reported OK if its artifacts are trustworthy: the recorder covered the
right window, every participant reported final metrics, and nobody left live orders in
the book.

#### Comparing and tuning

```bash
# Vary one parameter, aggregate repeats, and flag differences inside run-to-run noise
experiments/sweep.py <scenario> --param participants.lp1.config.averageQuantity \
    --values 250,500,1000 --repeat 3

# Compare runs; the varying parameter is detected by diffing their configs
experiments/compare.py experiments/results/_sweeps/*/run_001 \
    --metrics sweep.peak_impact_ticks --curve sweep.peak_impact_ticks
```

### Presets

Calibrated, launchable scenarios — the intended entry point for a UI or a demo.

```bash
experiments/presets.py list
experiments/presets.py verify          # runs each one and records whether it held up
experiments/presets.py manifest        # machine-readable index for a consumer
```

`normal_market`, `thin_liquidity`, `buy_sweep`, `large_buy_sweep`, `selloff`,
`liquidity_crisis`, `volatility_shock`, `market_maker_inventory_pressure`,
`execution_immediate`, `execution_sliced`, `spoofing_detection_demo`.

`presets/manifest.json` carries each preset's description, participants, and the
verdict from the last verification run, so a consumer can tell which are currently
trustworthy rather than assuming.

Scenarios under `experiments/scenarios/` are investigative by contrast — controls,
parameter ladders, A/B arms. Presets are the subset worth pressing a button on.

### Hypotheses

Every scenario states what it expects **before** it runs, and the analysis classifies
the outcome as consistent, partially consistent, or inconsistent. It is far too easy
to look at a synthetic market afterwards and narrate whatever it happened to do as the
intended result; stating the claim first is the only defence against that.

Several findings in this repository came from that check failing.

### Manipulation-detection demonstration

`spoofing_detection_demo` runs a layering actor alongside genuine participants in the
simulated market. Its subject is **detection**: which behavioural measures separate a
manipulative actor from honest ones, and which misleadingly do not.

The first anomaly score written for it ranked the honest liquidity provider as more
anomalous than the layering actor — the provider cancels 99.9% of its orders, because
continuous quoting means continuously requoting. That false positive is the point of
the scenario, and it is documented in the file.

This exists only for this synthetic engine and simulated participants.

### Demo web app

Three containers behind nginx: the engine, a FastAPI/WebSocket bridge, and a React
front end showing live candles and Level 2.

```bash
docker compose up -d     # then open http://localhost
```

The chart stays empty until participants are trading. Start some with a preset, or
directly:

```bash
docker exec -it order-matching-engine-cpp-server-1 \
  ./participant_runner --config /path/to/participant.json
```

To deploy elsewhere, rebuild the front end with your own URLs:

```bash
docker compose build \
  --build-arg REACT_APP_API_HTTP_URL=https://yourdomain.com \
  --build-arg REACT_APP_API_WS_URL=wss://yourdomain.com \
  frontend
```

### Known limitations

Recorded because they affect how results should be read:

- **No fundamental anchor.** No participant holds a view on value, so a large enough
  directional push reprices the market permanently and without bound. Correct for the
  participants present, but a property of who is in the market rather than a realistic
  depiction of one.
- **Thin books are erratic.** Scenarios built on sparse liquidity vary a great deal
  run to run. Use `sweep.py --repeat` before believing a difference; `volatility_shock`
  documents its own spread.
- **Reproducibility is behavioural, not bit-exact.** A fixed seed reproduces a
  participant's decision sequence, but engine order ids are random and thread
  interleaving is not deterministic, so aggregate metrics are stable while tail metrics
  are not.
- **Spread is set by order remainders.** The engine has no immediate-or-cancel type, so
  an aggressive order's unfilled remainder rests inside the spread. Effective spread is
  therefore driven by those remainders rather than by the provider's quotes.

## License

[MIT](LICENSE)
