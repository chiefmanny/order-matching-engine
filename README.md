# Order Matching Engine

A C++ order matching engine, a suite of simulated market participants that trade
against it, and an experiment harness that runs them together and measures what
happens.

The engine is real: every order, match, cancellation and fill in every experiment goes
through it over gRPC. Python orchestrates and analyses, and never simulates market
behavior itself.

## What's here

```
src/core/            order book + matching algorithm
src/grpc/            gRPC server and client
src/clients/         participant framework and the strategies built on it
experiments/         scenario runner, recorder, metrics, plots, presets
backend/ frontend/   FastAPI + React demo showing live candles and Level 2
```

## Market participants

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

## Running an experiment

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

### Comparing and tuning

```bash
# Vary one parameter, aggregate repeats, and flag differences inside run-to-run noise
experiments/sweep.py <scenario> --param participants.lp1.config.averageQuantity \
    --values 250,500,1000 --repeat 3

# Compare runs; the varying parameter is detected by diffing their configs
experiments/compare.py experiments/results/_sweeps/*/run_001 \
    --metrics sweep.peak_impact_ticks --curve sweep.peak_impact_ticks
```

## Presets

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

## Hypotheses

Every scenario states what it expects **before** it runs, and the analysis classifies
the outcome as consistent, partially consistent, or inconsistent. It is far too easy
to look at a synthetic market afterwards and narrate whatever it happened to do as the
intended result; stating the claim first is the only defence against that.

Several findings in this repository came from that check failing.

## Manipulation-detection demonstration

`spoofing_detection_demo` runs a layering actor alongside genuine participants in the
simulated market. Its subject is **detection**: which behavioural measures separate a
manipulative actor from honest ones, and which misleadingly do not.

The first anomaly score written for it ranked the honest liquidity provider as more
anomalous than the layering actor — the provider cancels 99.9% of its orders, because
continuous quoting means continuously requoting. That false positive is the point of
the scenario, and it is documented in the file.

This exists only for this synthetic engine and simulated participants.

## Tests

```bash
cmake -B build && cmake --build build
./build/participant_tests        # strategies, order tracking, tick math, event log
./build/book_invariant_tests     # order-book depth bookkeeping and the trade tape
experiments/tests/test_scenario.py
experiments/tests/test_metrics.py
```

`participant_tests` and the scenario tests need no gRPC installation, so the fast path
runs anywhere.

## Demo web app

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

## Known limitations

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
