# Market Demos on manan.com — Design

Status: approved, not yet implemented. Written 2026-09-17 on branch `master-website`.

Spans three repositories:

- `order-matching-engine` (this one) — the engine, participants, presets, experiment harness
- `finchart` — the charting engine, its matching-engine gRPC adapter, and its WASM web showcase
- `manan.com` — the Next.js personal site

Read `docs/HANDOFF.md` first for the state of this repo.

---

## Goal

Seven interactive essays on manan.com, each explaining one market phenomenon and
demonstrating it with a recorded session of the real matching engine, replayed in the
browser by the real finchart engine compiled to WebAssembly.

Every number quoted in an essay is a measured result from the same run the visitor is
watching.

## Non-goals

- No live engine backend. Demos are recorded replays served as static assets.
- No Canvas/WebGPU renderer. The existing terminal backend's ANSI output painted into
  a `<pre>` is the visual, shipped as-is. `backend-terminal`'s existing
  `html::ansi_to_html` does the conversion, so no terminal emulator is needed.
- No live-parameter sliders. The no-fundamental-anchor limitation (`docs/HANDOFF.md`
  limitation 1) means an unbounded intensity control produces a runaway, not a bigger
  version of the same market.
- No new participant strategies, no engine matching changes, no recalibration of the
  baseline.

---

## Why this is possible now

Three properties of the current code make the design work, and each was a fix made
during the 13-phase effort described in `docs/HANDOFF.md`:

1. **Per-subscriber fan-out.** `Subscribe` used to pop a single shared queue, so two
   subscribers split the trade tape. It now fans out per subscriber, which is what
   allows `market_recorder` and the finchart adapter to observe the *same* run
   completely and independently. Without it, the recording and the metrics would
   describe different halves of one market.
2. **`aggressor_is_buy` (proto tag 10).** Trade side is now unambiguous on the wire.
3. **Maker-priced trades.** Trades print at the resting order's price rather than the
   midpoint of the two limits, so impact and slippage in an essay measure the market
   rather than an engine artifact.

## The correctness problem this design must fix first

`finchart/examples-matching-engine` vendors its own copy of `matching_engine.proto`.
That copy is stale: it predates tags 7-10.

Wire compatibility is fine — the added tags are additive, so the old copy still
decodes. The semantic problem is not fine. `translate::fill_side` derives trade side
from field 4 (`buy`) on the documented assumption that `buy == true` means
buyer-initiated. The engine's proto now states the opposite explicitly:

> `bool buy = 4;` — legacy: true when the BID order was fully consumed.
> This is NOT the aggressor side -- use aggressor_is_buy.

The adapter carries a `--invert-trade-side` flag to hedge the guess. Every trade side
in every recording would therefore be labelled by an assumption that is documented as
wrong, and trade sides are precisely what the order-flow and delta views consume.

`aggressor_is_buy` resolves it. This must land before any recording is kept.

Note for implementation: the existing tests `fill_side_default_is_buyer_initiated` and
`fill_side_invert_flips_the_mapping` assert the incorrect mapping. They certify the
bug rather than catch it. Delete and replace them; do not amend around them.

---

## Architecture

```
preset.yaml
   |
   +- run.py --> engine container (:50051, published only for recording runs)
                   |
                   +-> market_recorder  --> trades.csv, level2.csv
                   |                          |
                   |                          +-> analyze.py --> summary.json, plots/*.png
                   |
                   +-> finchart adapter --> scenario .txt
                                                 |                 |
                                                 +--------+--------+
                                                          v
                                    manan.com/public/finchart/{scenarios,plots}/
                                    manan.com/public/finchart/pkg/  (WASM bundle)
                                                          |
                                                          v
                        [browser]  MarketChart --> player.ts --> WasmChart --> <pre>
```

One run produces both the recording and the evidence. This is the central property and
should not be traded away for convenience.

### Rejected alternative: offline CSV conversion

Converting `trades.csv` / `level2.csv` into finchart's `MarketEvent` text format in
Python would avoid publishing the port and avoid the timing questions entirely, and it
would let recordings be regenerated from archived runs.

It was rejected because it duplicates `translate.rs`'s semantics in a second language.
A stale second copy of exactly that translation is the bug described above. It also
leaves the gRPC integration unexercised, when "finchart consumes the engine's live
feed" is a substantial part of what makes the result worth showing.

### Rejected alternative: iframe embed

Embedding `finchart/web/` verbatim in an `<iframe>` is far less work, and upstream
finchart improvements would arrive free. Rejected because the iframe cannot see the
site's theme, is awkward to size responsively inside prose, traps keyboard focus
(pan/zoom are key-driven), and is hard to give an accessible name and a reduced-motion
path. `manan.com/AGENTS.md` has an explicit accessibility section; this is the wrong
place to save effort.

### Rejected alternative: npm package from wasm-pack

Publishing `finchart-web` as a versioned package is the clean boundary in principle,
but it is registry-or-git-dependency machinery plus wasm-pack/Next.js bundling friction
for a boundary with exactly one consumer.

---

## Upstream changes

### finchart

1. **Refresh the vendored proto** from `order-matching-engine/matching_engine.proto`,
   keeping the "vendored verbatim" header comment.
2. **`translate::fill_side` uses `aggressor_is_buy`.** Remove the `invert` parameter,
   the `--invert-trade-side` flag, its help text, and the tests asserting the old
   mapping. Add tests driven off `aggressor_is_buy`.
3. **Unattended recording.** Add `--duration <secs>` and `--headless` to
   `examples-matching-engine`. With `--duration`, the adapter exits and flushes the
   recording after that many seconds. With `--headless`, it skips TUI rendering
   entirely. Together they make an eight-scenario capture reproducible and let the
   recording window be aligned to the scenario's own window — which matters most for
   scenarios with markers, where a ragged start or end changes what the demo shows.
   Follow the existing arg-parsing test pattern (`replay_and_record_flags_capture_paths`).

### order-matching-engine

4. **`run.py --finchart-record` / `--finchart-bin`.** `DockerLauncher.setup` currently runs
   `docker run -d --name ... -v ...` with no `-p`; all processes share the container's
   network namespace via `docker exec`, so the engine is unreachable from the host. Add
   an opt-in `publish_port` that appends `-p 50051:50051` for recording runs only, and
   have `run.py` launch the host-side adapter at the same lifecycle point where
   `market_recorder` attaches. Launching it from `run.py` rather than from an external
   script removes a race: participants begin trading at `start_at` (earliest 1.0s), so
   an external poller could miss the opening of the tape. Normal experiment runs must
   be unchanged. Cover the argv construction in `experiments/tests/test_launcher.py`.

Nothing else in the engine changes. No new strategies, no matching changes, no
recalibration.

---

## The seven demos

| slug | preset(s) | the idea |
|---|---|---|
| `normal-market` | `normal_market` | what a functioning two-sided market looks like |
| `thin-liquidity` | `thin_liquidity` | the same order flow moves price much more when depth is shallow |
| `price-impact` | `large_buy_sweep` | what a large order does to price, and what it costs |
| `execution-styles` | `execution_immediate`, `execution_sliced` | two ways to work the same parent order, compared |
| `liquidity-crisis` | `liquidity_crisis` | what happens when the book withdraws |
| `inventory-risk` | `market_maker_inventory_pressure` | why a market maker skews quotes and eventually pulls a side |
| `spoofing-detection` | `spoofing_detection_demo` | surveillance fingerprints, and the false positive that matters |

Eight recordings, seven essays: `execution-styles` embeds both recordings and compares
them.

`spoofing-detection` must carry the finding from `docs/HANDOFF.md` limitation 7 — that a
naive cancel-ratio-plus-order-to-trade score ranks the *honest* liquidity provider as
more anomalous than the layering actor, because continuous quoting means continuous
requoting. That false positive is the scenario's actual lesson and the most
interesting thing on the page. Only `cancel_ratio x relative_size` was stable across
runs.

The three presets not used remain in the repo and are unaffected:

- `buy_sweep` and `selloff` are smaller or opposite-signed versions of the idea
  `price-impact` already covers with `large_buy_sweep`.
- `volatility_shock` is too variable run to run to anchor an essay on a single
  recording. `docs/HANDOFF.md` records its realised vol ratio as 7.9 +/- 5.7. It becomes a
  candidate once it has been re-verified with `sweep.py --repeat` and its thresholds
  derived from the distribution.

---

## Site structure

### Routes

All statically generated.

| route | contents |
|---|---|
| `/projects/market-microstructure` | preamble (what this is, how it was measured, what it does not model), then the seven demos listed compactly in the existing `ProjectList` idiom |
| `/projects/market-microstructure/[demo]` | one essay per demo |

These sit in the slot `manan.com/docs/architecture.md` already sanctions —
`/projects/[slug]`, "optional flagship project pages only" — rather than introducing a
new top-level section, so no documented routing convention is broken.

There is no separate methodology page. The index preamble carries the
hypothesis-then-verdict explanation and the honest limitations; essays affected by a
specific limitation carry a short footer note. The two limitations that must appear:

- **No fundamental anchor.** No participant holds a view on value, so a large enough
  directional push reprices the market permanently and without bound. Price impact
  accumulates rather than decaying. This is a property of who is in the market.
- **Thin books are erratic.** Scenarios built on sparse liquidity vary substantially
  run to run.

### Content model

`manan.com/AGENTS.md` requires structured content files separated from presentation,
and is explicitly dependency-averse. MDX was considered and rejected: the seven essays
are structurally uniform (every preset has the same shape — description, hypothesis,
metrics, verdict), and uniform structure is better served by typed data than by
free-form markup.

```ts
export type Demo = {
  slug: string;
  title: string;
  blurb: string;                  // one line, for the index
  presets: string[];              // preset name(s) behind this demo
  replay: { interval: number; tick: number; heatmap: boolean; window: number };
  recordings: { id: string; label: string }[];   // 2 for execution-styles
  hypothesis: string;             // generated
  sections: { heading: string; body: string[] }[];
  watchFor: string[];             // what to look for on the chart
  metrics: { label: string; value: string; note?: string }[];   // generated
  plots: { src: string; alt: string; caption: string }[];
  verdict: "consistent" | "inconsistent" | "undetermined";      // generated
  verifiedAt: string;                                            // generated
};
```

Files live in `manan.com/content/markets.ts`, matching the repo's one-file-per-domain
content convention (`content/projects.ts`, `content/notes.ts`).

`MarketVerdict` must carry all four values `analyze.py` emits (`analyze.py:60-66`):
`consistent`, `partially_consistent`, `inconsistent`, `undetermined`.

**Generated fields are not typed by hand.** A scaffold script reads
`experiments/presets/manifest.json` and the run's `summary.json` and emits the content
file with `hypothesis`, `metrics`, `verdict` and `verifiedAt` already populated. The
author writes only `sections`, `watchFor` and plot captions.

This is deliberate. The entire credibility of these pages rests on the numbers being
the measured ones, and hand-transcribing eight runs' metrics is how that quietly stops
being true.

### Components

| path | responsibility |
|---|---|
| `src/lib/finchart/wasm.ts` | lazy WASM instantiation, single-instance guard |
| `src/lib/finchart/player.ts` | ported from `finchart/web/main.js`: scenario fetch, event scheduling, play/pause/speed/seek, pan/zoom/follow. No React. |
| `src/components/MarketChart.tsx` | client component; paints `render_html` into a `<pre>`, sizing, controls |
| `src/components/MetricTable.tsx` | hypothesis, measured values, verdict |

Existing `Section.tsx` and the site's typography are reused. `projects.ts` gains a
flagship entry pointing at `/projects/market-microstructure`.

### Asset layout

Committed to `manan.com`, so the site builds and deploys with no Rust toolchain and no
finchart checkout:

```
public/finchart/
  pkg/            finchart_web_bg.wasm (~113 KB), finchart_web.js (~10 KB)
  scenarios/      8 recorded MarketEvent .txt files
  plots/          selected PNGs from each run's plots/ directory
```

Total weight is expected to stay well under a megabyte gzipped; `baseline.txt` is 50 KB
for 905 events. Measure the real preset recordings in phase 4 and revisit if a 45-60
second busy market produces substantially more than expected.

A documented script regenerates all of it. The commit is the source of truth for
deploys.

---

## Error handling

**The essay is complete without the chart.** Prose, metric table and plots are static
HTML from SSG. The terminal is progressive enhancement.

| failure | behaviour |
|---|---|
| WASM fails to instantiate | terminal region shows a short plain message; rest of page unaffected |
| scenario fetch fails or 404s | same |
| the WASM bundle fails to fetch | same |
| recording is truncated or empty | caught before commit by scenario validation (below), never reaches the site |

No error path may blank the page or throw during hydration.

---

## Accessibility and presentation

`manan.com/AGENTS.md` has explicit requirements; these are the ones this feature
strains.

- **Reduced motion.** Under `prefers-reduced-motion`, do not autoplay. Render the
  first frame and require an explicit Play.
- **Keyboard.** Pan/zoom/follow are key-driven in `main.js`. Every control must also
  be reachable as a real button; key-only controls are not acceptable. The terminal
  needs an accessible name and visible focus.
- **Theme.** The site has a `ThemeToggle` that publishes nothing through React
  context — it only mutates `html[data-theme]`. Painting into a `<pre>` makes this a
  non-problem: `.up` / `.down` and the chart background are ordinary CSS reading the
  site's existing tokens, so the theme follows the toggle with no JS bridge and no
  `MutationObserver`. This is the main reason the `<pre>` was chosen over a terminal
  emulator.
- **Mobile.** This is the real risk. An ANSI chart needs a minimum column count to be
  legible, and the site must work at narrow widths without overlap. Mitigate by
  reducing font size and column count first; if it still does not render usefully at
  ~400px, show a static first frame with a "best viewed wider" note rather than a
  broken chart. Decide with a real device check in phase 7, not by assumption.

---

## Testing

### finchart

- `translate.rs`: replace `fill_side_default_is_buyer_initiated` and
  `fill_side_invert_flips_the_mapping` with tests driven off `aggressor_is_buy`.
- `--duration` / `--headless` arg parsing, following the existing pattern.

### Scenario validation

The adapter already has an offline `--replay <file>` mode requiring no engine. Every
committed `.txt` is validated by replaying it and asserting a non-trivial rendered
frame and a plausible event count.

This is the gate that stops a truncated or silently empty recording reaching the site.
It exists because of a lesson already recorded in `docs/HANDOFF.md`: the Level 2 heatmap
once rendered a plausible image with axes, a colourbar and no data at all. An artifact
existing is not evidence it is right.

### order-matching-engine

- `experiments/tests/test_launcher.py` (new): `DockerLauncher.docker_run_argv()`
  appends the port mapping when `publish_port` is set and omits it by default.
- Existing suites (`participant_tests`, `book_invariant_tests`, both Python suites)
  must stay green. No engine behaviour changes here, so any movement is a regression.

### manan.com

- Gate: `npm run lint && npm run typecheck && npm run build`.
- **vitest** is added as the one new devDependency, covering `lib/finchart/player.ts`
  only: event scheduling, speed, seek, and the reduced-motion path. That module is the
  only genuinely stateful logic on the site; everything else is presentation and is
  covered by typecheck and build.

---

## Sequencing

Each phase is independently verifiable and leaves the tree working.

| # | phase | done when |
|---|---|---|
| 1 | finchart proto refresh + `aggressor_is_buy` | new tests pass; `--invert-trade-side` gone |
| 2 | adapter `--duration` / `--headless` | an unattended run produces a valid recording |
| 3 | engine `run.py --finchart-record` | the adapter records a live preset run from the host |
| 4 | record 8 scenarios, run `analyze.py` | 8 validated `.txt`, plus summary and plots per demo |
| 5 | port player, build `MarketChart`, ship **one** demo | `normal-market` deployed and working in a real browser |
| 6 | scaffold script + remaining 6 essays | all 7 pages build |
| 7 | theme, mobile, reduced motion, accessibility | verified at 400px and in both themes |
| 8 | homepage flagship entry in `projects.ts` | the site index points at it |

Phase 5 deliberately carries a single demo all the way to deployed before the other six
essays are written. The content is the expensive part and should not be written against
a player that has not survived contact with a real browser.

---

## Risks

| risk | handling |
|---|---|
| ANSI chart illegible on mobile | decided empirically in phase 7; static-frame fallback if needed |
| recordings larger than expected | measured in phase 4; shorten window or reduce heatmap capacity |
| recording window misaligned with scenario markers | `--duration` plus starting the adapter at a known point in `run.py`'s timeline |
| thin-book scenarios vary run to run | `liquidity-crisis` essay states the variance; do not present one run as typical |
| cross-repo drift (proto changes again) | the vendored proto keeps its provenance comment; scenario validation catches decode breakage |
| finchart `main.js` and the ported `player.ts` diverge | accepted. `main.js` stays the reference implementation for finchart's own showcase; the port is a one-time fork, not a maintained shared module |

## Open questions

None blocking. Two to settle during implementation:

- Which specific plots from each run's `plots/` directory earn a place in each essay.
  Decide in phase 4 by looking at the rendered output, not from the filenames.
- Whether the section needs its own entry in the site header or only a `projects.ts`
  entry. Decide in phase 8 once the pages exist. Note that `Header`'s four nav hrefs
  are page-relative hashes (`#projects`) that break on any non-root route, so they
  become root-absolute (`/#projects`) regardless.
