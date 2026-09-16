# Handoff

Context for picking this project up cold. Written at commit `b872fdd` on branch
`master-website`, after completing a 13-phase plan that built a market-participant
simulation suite and experiment harness on top of an existing C++ matching engine.

Read this, then `README.md` for usage.

---

## What this project is

A **market-microstructure laboratory**. The C++ matching engine is the market;
simulated C++ participants trade against it over gRPC; a Python harness runs them
together, records what happened, and measures it.

The original motivating question was: *what combination of simulated participants
actually creates convincing market behavior?* That question now has a measured answer
rather than an assumed one, and the answer is captured in
`experiments/baselines/normal_market.yaml`.

The eventual goal — **not yet started** — is an interactive website where the engine
is the market, participants generate the activity, and a UI launches known-good
scenarios. The presets are the interface that UI is meant to call.

---

## Current state

### Engine (`src/core`, `src/grpc`)

Working, with several correctness fixes made during this effort. Anyone touching the
engine should know these, because earlier code and any external consumer predate them:

- `LimitPrice::totalSize` was never initialised and never decremented on fills, so
  **every Level 2 depth number was undefined**. Fixed and covered by
  `book_invariant_tests`.
- `Subscribe` popped a single shared fill queue, so two subscribers **split the trade
  tape between them**. Replaced with per-subscriber fan-out.
- Trades printed at the **midpoint of the two limit prices** rather than the resting
  order's price, making slippage and impact measure an engine artifact. `OrderInternal`
  now carries an arrival sequence; the earlier order is the maker and sets the price.
- Order ids were drawn from the full unsigned range while `OrderID.id` is `int32` on
  the wire, so ids above 2³¹ **came back negative**. Now drawn from `[1, INT32_MAX]`.
- `FillNotification` gained `timestamp_ns`, `maker_order_id`, `taker_order_id`,
  `aggressor_is_buy` as **additive proto tags 7–10**. Fields 1–6 are unchanged.
- `WatchSymbol`'s BBO cadence and the registered symbol list are now CLI flags
  (`--bbo-interval-ms`, `--symbols`). Engine stdout is behind `--verbose`.

### Participant framework (`src/clients/common`)

One executable, `participant_runner`, becomes whichever participant its JSON config
names. Adding a participant is one strategy file plus one registry line.

- `OrderGateway` — narrow abstract interface for submit/cancel/replace. This is what
  keeps strategies free of gRPC, so every strategy is unit-tested against a fake
  gateway with no engine, no network and no threads.
- `EngineSession` — the only place a participant knows gRPC exists. **Enforces order
  ownership**, because the engine performs no ownership check at all.
- `ParticipantRuntime` — four threads (fill stream, BBO stream, L2 poller, decision
  loop). Guarantees owned orders are cancelled at shutdown; exiting with live orders
  is exit code 4.
- `EventLog` — JSONL to stdout, one object per line. The harness reads this and
  nothing else from a participant's stdout; human output goes to stderr.

Seven strategies: `liquidity_provider`, `noise_trader`, `market_maker`,
`directional_trader`, `parent_executor` (with `sweep` as an alias for its immediate
mode), `layering_demo`, and `echo` (a smoke test).

`market_recorder` is a separate executable — the independent observer. It deliberately
does **not** reuse `EngineSession`, because a participant's session filters fills to
owned orders while a recorder wants the complete unfiltered tape.

### Harness (`experiments/`)

`run.py` orchestrates, `analyze.py` computes metrics and judges hypotheses, `sweep.py`
varies one parameter with repeat aggregation, `compare.py` tabulates runs and plots
curves, `plots.py` renders seven diagnostics, `presets.py` verifies and indexes the
preset set.

Eight metrics modules under `experiments/metrics/`: baseline, order_flow (including
Cont–Kukanov–Stoikov OFI), participant, sweep, market_maker (adverse selection),
regime (before/after a marker), directional (temporary vs permanent impact),
manipulation (surveillance fingerprints).

20 investigative scenarios, 2 calibrated baselines, **11 verified presets**.

### Tests

```
./build/participant_tests        # 122 assertions across 11 strategy/infra suites
./build/book_invariant_tests     # engine depth bookkeeping and trade tape
experiments/tests/test_scenario.py
experiments/tests/test_metrics.py
```

`participant_tests` and the Python suites need no gRPC installation. The gRPC binaries
build in Docker (`Dockerfile.cpp`).

---

## The single most important design decision

**The liquidity provider's inventory-driven reference shift is load-bearing.**

Without it the provider is an infinitely elastic wall: it refills every consumed level
at the same price forever, so the mid it quotes is a fixed point that no amount of
order flow can move. This was measured, not theorised — price moved 12 ticks with
noise alone and only 2 ticks once a rigid provider was added, while the provider
silently absorbed +1007 shares without ever responding.

With `inventorySkewTicksPerUnit: 4.0`, accumulating inventory shifts the whole ladder,
and directional pressure moves price instead of being absorbed. Every scenario that
demonstrates price impact depends on this.

If you ever see price refusing to move in a new scenario, check this parameter first.

---

## Limitations

These are recorded because they affect how results should be read, not as a to-do list.

### 1. No fundamental anchor

No participant holds a view on value. There is no informed trader or arbitrageur, so a
large enough directional push reprices the market **permanently and without bound**.
Measured: at one intensity the buyer was 70% of volume, moved price 282 ticks, and was
still climbing 1250 ticks later — persistence above 1, meaning price *accelerated
after the pressure stopped*.

The feedback loop is: buyer lifts the book → provider forced short → inventory skew
raises its quotes → raises the mid → raises the provider's own re-centring reference.
Nothing pulls back.

This is correct for the participants present but is a property of *who is in the
market* rather than a realistic market. **It matters most for a UI**: an intensity
slider pushed past the calibrated setting will produce a runaway, not a bigger version
of the same thing.

### 2. Thin books are erratic

Scenarios built on sparse liquidity vary enormously run to run. `volatility_shock`
documents its own spread (realised vol ratio 7.9 ± 5.7). Always use
`sweep.py --repeat` before believing a difference; the tool flags when variation across
values is no larger than variation within a value.

### 3. Reproducibility is behavioural, not bit-exact

A fixed seed reproduces a participant's decision sequence, but engine order ids are
random and thread interleaving is not deterministic. Aggregate metrics are stable
across same-seed runs (2.46 vs 2.39 trades/s); tail metrics are not (max spread 5 vs
31 ticks).

### 4. Spread is set by order remainders, not by quotes

The engine has no immediate-or-cancel type, so an aggressive order's unfilled remainder
**rests inside the spread**. Realised spread is therefore driven by those remainders
rather than by the provider's quoted spread — widening `halfSpreadTicks` from 1 to 3
barely moved it (1.73 → 1.67 ticks).

`directional_trader` works around this with `residualCancelMs`, which cancels the
remainder of a marketable order. The noise trader deliberately does **not**, because
adding it would change what the noise trader means and invalidate the frozen baseline.

### 5. Matching is asynchronous and there are no market orders

The matching algorithm is a background thread polling every 1ms. Every order rests in
the book first; a "marketable" order is a limit that happens to cross. This is why
sweeps measure fills over a sequence rather than atomically, and why residual
cancellation matters.

### 6. No `ModifyOrder` RPC

`OrderBook::modifyLimitOrder` exists in core but is not exposed. Participants use
cancel/replace, which is realistic (it surrenders queue position) and needed no engine
change. Revisit only if a strategy proves it needs in-place size reduction.

### 7. Surveillance metrics are weaker than they look

In `spoofing_detection_demo`, only `cancel_ratio × relative_size` proved stable across
runs (2.83× and 2.92× separation). Two other approaches failed:

- Cancel ratio + order-to-trade + size ranked the **honest liquidity provider as more
  anomalous than the layering actor** — it cancels 99.9% of orders because continuous
  quoting means continuously requoting. That false positive is the scenario's actual
  lesson.
- Lifetime regularity gave 6.43× separation on one run and inverted on the next,
  because a participant's orders are not one population (short-lived layers *and*
  longer-resting opposing orders in the same coefficient of variation).

Per-order-type regularity might work. One number per participant does not.

### 8. `order_book_tests` hangs

The legacy multithreaded stress suite is killed partway through
`randomized_orderbook_test_buy_trend` (exit 137). **This is pre-existing** — verified
by building unmodified HEAD code in a scratch directory and reproducing it exactly. It
creates ~10,000 distinct price levels from a continuous distribution across 5 threads.
It is currently a broken gate; either shrink its scale or drop it from the default set.

### 9. Local builds need Docker for anything gRPC

No local gRPC installation on the development machine, so `matching_engine_server`,
`participant_runner` and `market_recorder` build only in Docker. The fast test path
(`participant_tests`, Python suites) runs natively. `run.py --backend local` exists for
a machine that does have gRPC.

### 10. Docker signal handling is fiddly

`docker exec` does not forward SIGTERM into the container, and the slim runtime image
has no `/bin/kill`. The launcher records each process's in-container PID to the shared
mount and signals via `docker exec ... sh -c 'kill ...'`. If you change the base image
or the launcher, re-verify that participants still cancel their orders on shutdown —
the failure mode is silent.

---

## Things that were deliberately not done

- **The web UI.** Explicitly out of scope for the plan. The presets are its intended
  interface.
- **A `volatility_shock_client` or similar.** Market events are compositions of
  ordinary participants. If a future event seems to need a bespoke actor, that is a
  signal the composition is wrong.
- **`thin` as a liquidity-provider mode.** A thin book is fewer levels and smaller
  size — configuration already expresses it.
- **Research-grade fidelity.** Almgren–Chriss and Avellaneda–Stoikov are motivations,
  not implementations. There is no volatility estimate, no risk-aversion parameter, no
  optimal trajectory. The modes are named strategies whose relative cost is *measured*.

---

## Possible next steps

Roughly in order of value.

### Verify the finchart integration

`/Users/manan/Code/finchart` has an adapter (`examples-matching-engine`) that **vendors
its own reconstructed copy** of `matching_engine.proto`. The proto changes here were
additive (tags 7–10), so it should still decode — but this has **not been tested**.

Worth doing early, and there is a bonus: finchart's adapter plan documents the
aggressor-side ambiguity as unresolvable without a runnable engine. `aggressor_is_buy`
resolves it. Its `--invert-trade-side` flag and the guess it defends against can
probably both go.

### Build the web UI against the presets

The original goal. `experiments/presets/manifest.json` is designed for this: it carries
each preset's name, description, participants, markers, duration, and the verdict from
its last verification.

Two cautions. First, the no-anchor limitation — do not expose a raw intensity slider
without bounding it to calibrated ranges. Second, presets take 45–60 seconds of real
time; a UI needs to stream results rather than wait.

### Add a value-anchored participant

This is the principled fix for limitation 1. A participant with a view on fair value
that trades against deviations would make price impact decay rather than accumulate,
and would make the market's response to a shock realistic rather than monotonic.

It would change the calibrated baseline, so it needs recalibration and re-verification
of every preset.

### Tighten the surveillance demonstration

Per-order-type regularity (cluster a participant's orders by size or by strategy
intent before measuring lifetime dispersion) is the obvious next attempt at limitation
7. The current stable metric works but is coarse.

### Repeats on the variable scenarios

Several hypotheses currently rest on single runs. `volatility_shock`,
`liquidity_crisis` and anything built on a thin book would benefit from being verified
with `--repeat 5` and having their thresholds derived from the distribution rather than
from one observation.

### Multi-symbol

The engine accepts `--symbols AAPL,MSFT` and maintains a separate book and matching
thread per symbol, but **nothing has been tested with more than one**. The harness
assumes a single symbol throughout (`scenario.symbol` is scalar).

### Housekeeping

- Fix or retire `order_book_tests` (limitation 8) — a permanently failing gate is worse
  than no gate.
- CI. The fast path (`participant_tests`, `book_invariant_tests`, both Python suites)
  runs in seconds without gRPC and would catch most regressions.
- The `backend/live_candlestick.py` demo still builds candles from **midprice polling**
  rather than from executions. It predates this work and disagrees with
  `experiments/candles.py`. Either point it at the trade tape or note the discrepancy.

---

## Working habits that paid off

Worth continuing, because several real findings came from them:

- **State the hypothesis before the run.** `analyze.py` classifies the outcome, and a
  missing metric is `undetermined` rather than a silent pass. Multiple genuine findings
  came from a hypothesis failing — the slicing experiment, the volatility shock being
  too weak, the surveillance false positive.
- **Do not tune a threshold until it passes.** Where a threshold was adjusted, the
  scenario records the measured distribution it came from. Where the effect was inside
  the noise, the scenario says so instead.
- **Look at the plot, not just the file.** The Level 2 heatmap rendered a plausible
  image with axes, a colourbar and no data at all, because `np.maximum` propagates NaN.
  The artifact existing is not evidence it is right.
- **Check against unmodified HEAD before claiming a regression.** The `order_book_tests`
  hang looked like one and was not.
