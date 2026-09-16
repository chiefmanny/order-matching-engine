#include "fake_gateway.h"
#include "strategies/liquidity_provider.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>

namespace {

constexpr double kTick = 0.01;

// Assembles the pieces a StrategyContext needs, with no engine and no threads.
struct Fixture {
    MarketState      market;
    OrderTracker     tracker;
    PositionTracker  position;
    RandomSource     rng{42};
    std::ostringstream sink;
    EventLog         log{"test_run", "lp", sink};
    FakeGateway      gateway{tracker};
    nlohmann::json   params;
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

    StrategyContext ctx() {
        return StrategyContext{market, tracker, position, rng, gateway, log,
                               params, now, kTick, 0.0};
    }
    void advance(int ms) { now += std::chrono::milliseconds(ms); }
};

int failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) { std::cout << "  [FAIL] " << what << std::endl; ++failures; }
}

// --------------------------------------------------------------------------
// Ladder construction (pure function, no strategy state)
// --------------------------------------------------------------------------

void test_ladder_geometry() {
    LadderParams p;
    p.referencePrice    = 500.00;
    p.bidLevels         = 10;
    p.askLevels         = 10;
    p.halfSpreadTicks   = 1;
    p.levelSpacingTicks = 5;
    p.averageQuantity   = 500;

    RandomSource rng(1);
    auto ladder = LiquidityProvider::buildLadder(p, 500.00, kTick, rng);

    check(ladder.size() == 20, "10x10 ladder produces 20 levels");

    std::vector<double> bids, asks;
    for (const auto& l : ladder) (l.buy ? bids : asks).push_back(l.price);
    check(bids.size() == 10 && asks.size() == 10, "ten levels per side");

    // Best bid/ask sit halfSpreadTicks from the reference => a 2-tick spread.
    check(std::fabs(bids.front() - 499.99) < 1e-9, "best bid is one tick below reference");
    check(std::fabs(asks.front() - 500.01) < 1e-9, "best ask is one tick above reference");
    check(std::fabs((asks.front() - bids.front()) - 0.02) < 1e-9, "spread is two ticks");

    // Levels step by levelSpacingTicks.
    check(std::fabs(bids[1] - 499.94) < 1e-9, "second bid is five ticks below the first");
    check(std::fabs(asks[1] - 500.06) < 1e-9, "second ask is five ticks above the first");
    check(std::fabs(bids.back() - 499.54) < 1e-9, "deepest bid lands where the spacing implies");

    for (const auto& l : ladder) {
        check(l.size == 500, "rigid ladder: every level is exactly averageQuantity");
        // Prices must land exactly on the tick grid, or the engine creates a new
        // price level per floating-point wobble.
        const double ticks = l.price / kTick;
        check(std::fabs(ticks - std::llround(ticks)) < 1e-6, "price is on the tick grid");
    }
    std::cout << "  ladder geometry checked" << std::endl;
}

void test_quantity_variance_is_bounded_and_seed_reproducible() {
    LadderParams p;
    p.referencePrice  = 500.00;
    p.bidLevels = 20; p.askLevels = 20;
    p.averageQuantity = 500;
    p.quantityVariance = 150;

    RandomSource rngA(7), rngB(7), rngC(8);
    auto a = LiquidityProvider::buildLadder(p, 500.00, kTick, rngA);
    auto b = LiquidityProvider::buildLadder(p, 500.00, kTick, rngB);
    auto c = LiquidityProvider::buildLadder(p, 500.00, kTick, rngC);

    bool varied = false, inBounds = true;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].size != 500) varied = true;
        if (a[i].size < 350 || a[i].size > 650) inBounds = false;
    }
    check(varied, "quantityVariance actually varies level sizes");
    check(inBounds, "level sizes stay within averageQuantity +/- variance");

    bool sameSeedMatches = true, diffSeedDiffers = false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].size != b[i].size) sameSeedMatches = false;
        if (a[i].size != c[i].size) diffSeedDiffers = true;
    }
    check(sameSeedMatches, "same seed reproduces an identical ladder");
    check(diffSeedDiffers, "a different seed produces a different ladder");
    std::cout << "  quantity variance checked" << std::endl;
}

void test_max_distance_truncates_ladder() {
    LadderParams p;
    p.referencePrice    = 500.00;
    p.bidLevels = 50; p.askLevels = 50;
    p.halfSpreadTicks = 1; p.levelSpacingTicks = 1;
    p.maxDistanceFromReference = 0.10;  // 10 ticks

    RandomSource rng(1);
    auto ladder = LiquidityProvider::buildLadder(p, 500.00, kTick, rng);

    check(!ladder.empty(), "truncated ladder is not empty");
    for (const auto& l : ladder) {
        check(std::fabs(l.price - 500.00) <= 0.10 + 1e-9,
              "no level sits beyond maxDistanceFromReference");
    }
    check(ladder.size() == 20, "10 levels per side survive a 10-tick cap");
    std::cout << "  max distance checked" << std::endl;
}

void test_low_reference_price_never_goes_negative() {
    LadderParams p;
    p.referencePrice = 0.05;
    p.bidLevels = 20; p.askLevels = 2;
    p.halfSpreadTicks = 1; p.levelSpacingTicks = 1;

    RandomSource rng(1);
    auto ladder = LiquidityProvider::buildLadder(p, 0.05, kTick, rng);
    for (const auto& l : ladder) check(l.price > 0.0, "no level is priced at or below zero");
    std::cout << "  low-price guard checked" << std::endl;
}

// --------------------------------------------------------------------------
// Parameter validation
// --------------------------------------------------------------------------

void test_param_validation() {
    auto rejects = [](nlohmann::json j, const std::string& why) {
        try {
            LadderParams::fromJson(j);
            std::cout << "  [FAIL] should have rejected: " << why << std::endl;
            ++failures;
        } catch (const std::runtime_error&) { /* expected */ }
    };

    rejects(nlohmann::json::object(), "missing referencePrice");
    rejects({{"referencePrice", -1.0}}, "negative referencePrice");
    rejects({{"referencePrice", 100.0}, {"halfSpreadTicks", 0}}, "zero half-spread would self-cross");
    rejects({{"referencePrice", 100.0}, {"levelSpacingTicks", 0}}, "zero level spacing");
    rejects({{"referencePrice", 100.0}, {"averageQuantity", 0}}, "zero quantity");
    rejects({{"referencePrice", 100.0}, {"averageQuantity", 100}, {"quantityVariance", 100}},
            "variance >= averageQuantity could yield a zero size");
    rejects({{"referencePrice", 100.0}, {"recenterOnMid", true}}, "recenter without a threshold");
    // An unknown mode must fail loudly rather than silently acting normal.
    // (`withdraw` was rejected here until Phase 10 implemented it; it is now valid
    // and is covered by the withdrawal tests below.)
    rejects({{"referencePrice", 100.0}, {"mode", "nonsense"}}, "unknown mode");
    rejects({{"referencePrice", 100.0}, {"inventorySkewTicksPerUnit", -1.0}},
            "negative skew coefficient would amplify inventory into a runaway");

    LadderParams ok = LadderParams::fromJson({{"referencePrice", 500.0}, {"bidLevels", 3}});
    check(ok.bidLevels == 3, "valid config parses");
    check(ok.askLevels == 10, "unspecified fields take their defaults");
    std::cout << "  parameter validation checked" << std::endl;
}

// --------------------------------------------------------------------------
// Reconciliation behavior
// --------------------------------------------------------------------------

void test_seeds_full_ladder_on_first_tick() {
    Fixture f;
    f.params = {{"referencePrice", 500.00}, {"bidLevels", 10}, {"askLevels", 10},
                {"halfSpreadTicks", 1}, {"levelSpacingTicks", 5}, {"averageQuantity", 500}};
    LiquidityProvider lp;
    auto c1 = f.ctx(); lp.onStart(c1);

    check(f.gateway.submissions.empty(), "onStart places no orders by itself");
    auto c2 = f.ctx(); lp.onTick(c2);

    check(f.gateway.submissions.size() == 20, "first tick submits all 20 levels");
    check(f.tracker.activeCount() == 20, "20 orders are live");

    std::set<double> prices;
    int buys = 0;
    for (const auto& s : f.gateway.submissions) {
        prices.insert(s.price);
        if (s.buy) ++buys;
    }
    check(prices.size() == 20, "every level has a distinct price");
    check(buys == 10, "ten of them are bids");
    std::cout << "  ladder seeding checked" << std::endl;
}

void test_steady_state_does_not_churn() {
    Fixture f;
    f.params = {{"referencePrice", 500.00}, {"bidLevels", 5}, {"askLevels", 5}};
    LiquidityProvider lp;
    auto c1 = f.ctx(); lp.onStart(c1);
    auto c2 = f.ctx(); lp.onTick(c2);
    const size_t afterSeed = f.gateway.submissions.size();

    // A settled ladder must sit still: no resubmits, no cancels, no churn.
    for (int i = 0; i < 25; ++i) { f.advance(100); auto c = f.ctx(); lp.onTick(c); }

    check(f.gateway.submissions.size() == afterSeed,
          "no further orders once the ladder is complete");
    check(f.gateway.canceled.empty(), "a settled ladder cancels nothing");
    std::cout << "  steady state checked" << std::endl;
}

void test_replenishes_exactly_once_after_a_fill() {
    Fixture f;
    f.params = {{"referencePrice", 500.00}, {"bidLevels", 3}, {"askLevels", 3},
                {"replenishDelayMs", 400}};
    LiquidityProvider lp;
    auto c1 = f.ctx(); lp.onStart(c1);
    auto c2 = f.ctx(); lp.onTick(c2);
    const size_t afterSeed = f.gateway.submissions.size();
    check(afterSeed == 6, "seeded six levels");

    // Fully consume one resting order, and notify the strategy as the runtime would.
    const auto victim = f.gateway.submissions.front();
    f.gateway.fill(victim.id, victim.size);
    Fill fill{victim.id, victim.price, victim.size, victim.buy, true, 0};
    auto c3 = f.ctx(); lp.onFill(c3, fill);

    // Before the delay elapses, the level stays empty.
    f.advance(100);
    auto c4 = f.ctx(); lp.onTick(c4);
    check(f.gateway.submissions.size() == afterSeed,
          "no replenish before replenishDelayMs has elapsed");

    // After it elapses, exactly one replacement appears at the same price.
    f.advance(500);
    auto c5 = f.ctx(); lp.onTick(c5);
    check(f.gateway.submissions.size() == afterSeed + 1,
          "exactly one replenishment after the delay");
    const auto& replacement = f.gateway.submissions.back();
    check(std::fabs(replacement.price - victim.price) < 1e-9,
          "replenishment is at the consumed level's price");
    check(replacement.buy == victim.buy, "replenishment is on the same side");

    // And it must not keep replenishing on subsequent ticks.
    for (int i = 0; i < 10; ++i) { f.advance(100); auto c = f.ctx(); lp.onTick(c); }
    check(f.gateway.submissions.size() == afterSeed + 1,
          "the level is refilled once, not repeatedly");
    std::cout << "  replenishment checked" << std::endl;
}

void test_partial_fill_does_not_replenish() {
    Fixture f;
    f.params = {{"referencePrice", 500.00}, {"bidLevels", 2}, {"askLevels", 2}};
    LiquidityProvider lp;
    auto c1 = f.ctx(); lp.onStart(c1);
    auto c2 = f.ctx(); lp.onTick(c2);
    const size_t afterSeed = f.gateway.submissions.size();

    // A partially filled order is still resting; adding another would double the
    // level's displayed size.
    const auto victim = f.gateway.submissions.front();
    f.gateway.fill(victim.id, victim.size / 2);
    Fill fill{victim.id, victim.price, victim.size / 2, victim.buy, true, 0};
    auto c3 = f.ctx(); lp.onFill(c3, fill);

    f.advance(1000);
    auto c4 = f.ctx(); lp.onTick(c4);
    check(f.gateway.submissions.size() == afterSeed,
          "a partially filled level is not topped up");
    std::cout << "  partial fill checked" << std::endl;
}

void test_recentering_moves_the_ladder() {
    Fixture f;
    f.params = {{"referencePrice", 500.00}, {"bidLevels", 3}, {"askLevels", 3},
                {"recenterOnMid", true}, {"recenterThresholdTicks", 10}};
    LiquidityProvider lp;
    auto c1 = f.ctx(); lp.onStart(c1);
    auto c2 = f.ctx(); lp.onTick(c2);
    const size_t afterSeed = f.gateway.submissions.size();

    // A small drift must NOT move the ladder.
    f.market.bestBid = 500.02; f.market.bestAsk = 500.04;
    f.advance(100); auto c3 = f.ctx(); lp.onTick(c3);
    check(f.gateway.canceled.empty(), "a sub-threshold drift does not recenter");

    // A large drift must move it: old levels cancel, new ones appear around the mid.
    f.market.bestBid = 501.00; f.market.bestAsk = 501.02;
    f.advance(100); auto c4 = f.ctx(); lp.onTick(c4);
    check(!f.gateway.canceled.empty(), "crossing the threshold cancels stale levels");

    f.advance(100); auto c5 = f.ctx(); lp.onTick(c5);
    check(f.gateway.submissions.size() > afterSeed, "new levels are placed after recentering");

    bool nearNewMid = false;
    for (size_t i = afterSeed; i < f.gateway.submissions.size(); ++i) {
        if (std::fabs(f.gateway.submissions[i].price - 501.01) < 0.10) nearNewMid = true;
    }
    check(nearNewMid, "replacement levels are centered on the new mid");
    std::cout << "  recentering checked" << std::endl;
}

void test_rejected_submission_is_retried() {
    Fixture f;
    f.params = {{"referencePrice", 500.00}, {"bidLevels", 2}, {"askLevels", 2}};
    LiquidityProvider lp;
    auto c1 = f.ctx(); lp.onStart(c1);

    f.gateway.rejectNextSubmissions(4);   // the whole first pass is rejected
    auto c2 = f.ctx(); lp.onTick(c2);
    check(f.gateway.submissions.empty(), "all four submissions were rejected");
    check(f.tracker.rejectionCount() == 4, "rejections are counted");

    // The reconciler must keep trying rather than giving up on those levels.
    f.advance(100); auto c3 = f.ctx(); lp.onTick(c3);
    check(f.gateway.submissions.size() == 4, "rejected levels are retried on the next tick");
    std::cout << "  rejection retry checked" << std::endl;
}

// --------------------------------------------------------------------------
// Inventory-driven reference shift
// --------------------------------------------------------------------------

void test_inventory_skew_sign_and_magnitude() {
    LadderParams p;
    p.referencePrice = 500.00;
    p.averageQuantity = 500;
    p.inventorySkewTicksPerUnit = 2.0;   // 2 ticks per 500 shares
    p.maxInventorySkewTicks = 50;

    check(LiquidityProvider::inventorySkewTicks(p, 0) == 0, "flat inventory => no shift");

    // Long inventory must shift the ladder DOWN (positive skew). Getting this sign
    // backwards would amplify inventory into a runaway instead of shedding it.
    check(LiquidityProvider::inventorySkewTicks(p, 500) == 2,
          "long 500 shares => shift down 2 ticks");
    check(LiquidityProvider::inventorySkewTicks(p, 1000) == 4,
          "long 1000 shares => shift down 4 ticks");
    check(LiquidityProvider::inventorySkewTicks(p, -1000) == -4,
          "short 1000 shares => shift up 4 ticks");
    check(LiquidityProvider::inventorySkewTicks(p, 250) == 1, "half a unit rounds to 1 tick");
    std::cout << "  inventory skew sign and magnitude checked" << std::endl;
}

void test_inventory_skew_is_capped_and_disabled_by_default() {
    LadderParams p;
    p.referencePrice = 500.00;
    p.averageQuantity = 500;

    // Default: no response at all, so existing rigid-ladder scenarios are unchanged.
    check(LiquidityProvider::inventorySkewTicks(p, 100000) == 0,
          "skew is disabled by default (rigid wall preserved)");

    p.inventorySkewTicksPerUnit = 2.0;
    p.maxInventorySkewTicks = 10;
    check(LiquidityProvider::inventorySkewTicks(p, 1000000) == 10, "skew is capped above");
    check(LiquidityProvider::inventorySkewTicks(p, -1000000) == -10, "skew is capped below");

    p.inventoryTarget = 1000;
    check(LiquidityProvider::inventorySkewTicks(p, 1000) == 0,
          "no shift when inventory sits at its target");
    std::cout << "  inventory skew cap and target checked" << std::endl;
}

void test_ladder_moves_down_as_inventory_grows() {
    Fixture f;
    f.params = {{"referencePrice", 500.00}, {"bidLevels", 3}, {"askLevels", 3},
                {"averageQuantity", 500}, {"inventorySkewTicksPerUnit", 2.0}};
    LiquidityProvider lp;
    auto c1 = f.ctx(); lp.onStart(c1);
    auto c2 = f.ctx(); lp.onTick(c2);

    auto bestBidOf = [&](size_t from) {
        double best = 0.0;
        for (size_t i = from; i < f.gateway.submissions.size(); ++i)
            if (f.gateway.submissions[i].buy)
                best = std::max(best, f.gateway.submissions[i].price);
        return best;
    };
    const double bidBefore = bestBidOf(0);
    check(std::fabs(bidBefore - 499.99) < 1e-9, "starts quoting at the unshifted reference");

    // Simulate the provider being lifted: it buys 1000 shares.
    f.position.onFill(true, 1000, 500.00);
    const size_t mark = f.gateway.submissions.size();
    f.advance(100); auto c3 = f.ctx(); lp.onTick(c3);
    f.advance(100); auto c4 = f.ctx(); lp.onTick(c4);

    check(!f.gateway.canceled.empty(), "the stale ladder is cancelled on the shift");
    const double bidAfter = bestBidOf(mark);
    check(bidAfter > 0.0, "a new ladder is quoted after the shift");
    // 1000 shares / 500 per unit * 2 ticks = 4 ticks down.
    check(std::fabs(bidAfter - 499.95) < 1e-9,
          "long 1000 shares moves the best bid down 4 ticks (got " +
              std::to_string(bidAfter) + ")");
    std::cout << "  ladder shifts down on long inventory checked" << std::endl;
}

void test_ladder_moves_up_when_short() {
    Fixture f;
    f.params = {{"referencePrice", 500.00}, {"bidLevels", 3}, {"askLevels", 3},
                {"averageQuantity", 500}, {"inventorySkewTicksPerUnit", 2.0}};
    LiquidityProvider lp;
    auto c1 = f.ctx(); lp.onStart(c1);
    auto c2 = f.ctx(); lp.onTick(c2);

    f.position.onFill(false, 1000, 500.00);   // sold 1000 => short
    const size_t mark = f.gateway.submissions.size();
    f.advance(100); auto c3 = f.ctx(); lp.onTick(c3);
    f.advance(100); auto c4 = f.ctx(); lp.onTick(c4);

    double bestAsk = 1e9;
    for (size_t i = mark; i < f.gateway.submissions.size(); ++i)
        if (!f.gateway.submissions[i].buy)
            bestAsk = std::min(bestAsk, f.gateway.submissions[i].price);
    check(std::fabs(bestAsk - 500.05) < 1e-9,
          "short 1000 shares moves the best ask up 4 ticks (got " +
              std::to_string(bestAsk) + ")");
    std::cout << "  ladder shifts up on short inventory checked" << std::endl;
}

void test_no_shift_without_inventory_change() {
    Fixture f;
    f.params = {{"referencePrice", 500.00}, {"bidLevels", 3}, {"askLevels", 3},
                {"averageQuantity", 500}, {"inventorySkewTicksPerUnit", 2.0}};
    LiquidityProvider lp;
    auto c1 = f.ctx(); lp.onStart(c1);
    auto c2 = f.ctx(); lp.onTick(c2);
    const size_t afterSeed = f.gateway.submissions.size();

    // Inventory small enough to round to zero ticks must not churn the ladder.
    f.position.onFill(true, 100, 500.00);   // 100/500 * 2 = 0.4 ticks -> 0
    for (int i = 0; i < 10; ++i) { f.advance(100); auto c = f.ctx(); lp.onTick(c); }

    check(f.gateway.submissions.size() == afterSeed,
          "a sub-tick inventory change does not requote the ladder");
    check(f.gateway.canceled.empty(), "no churn from a sub-tick inventory change");
    std::cout << "  sub-tick inventory stability checked" << std::endl;
}

// --------------------------------------------------------------------------
// Liquidity withdrawal (mode: withdraw)
// --------------------------------------------------------------------------

// The fixture's elapsedSeconds is fixed at 0, but withdrawal is driven by elapsed
// time, so these drive it explicitly.
static StrategyContext ctxAt(Fixture& f, double elapsedSeconds) {
    return StrategyContext{f.market, f.tracker, f.position, f.rng, f.gateway, f.log,
                           f.params, f.now, kTick, elapsedSeconds};
}

void test_withdraw_mode_is_accepted_and_validated() {
    LadderParams p = LadderParams::fromJson(
        {{"referencePrice", 500.0}, {"mode", "withdraw"},
         {"withdrawAfterSeconds", 20}, {"withdrawDurationMs", 4000}});
    check(p.mode == "withdraw", "withdraw mode parses");
    check(p.withdrawOrder == "inside_out", "withdrawal defaults to inside-out");

    auto rejects = [](nlohmann::json j, const std::string& why) {
        try {
            LadderParams::fromJson(j);
            std::cout << "  [FAIL] should have rejected: " << why << std::endl;
            ++failures;
        } catch (const std::runtime_error&) {}
    };
    rejects({{"referencePrice", 500.0}, {"mode", "withdraw"},
             {"withdrawOrder", "sideways"}}, "unknown withdrawal order");
    rejects({{"referencePrice", 500.0}, {"mode", "withdraw"},
             {"withdrawAfterSeconds", -1}}, "negative withdrawal delay");
    std::cout << "  withdraw parameters checked" << std::endl;
}

void test_withdrawal_does_not_start_early() {
    Fixture f;
    f.params = {{"referencePrice", 500.00}, {"bidLevels", 5}, {"askLevels", 5},
                {"mode", "withdraw"}, {"withdrawAfterSeconds", 20},
                {"withdrawDurationMs", 2000}};
    LiquidityProvider lp;
    auto c0 = ctxAt(f, 0.0); lp.onStart(c0);
    for (int i = 0; i < 10; ++i) { f.advance(100); auto c = ctxAt(f, i * 0.1); lp.onTick(c); }

    check(f.tracker.activeCount() == 10, "the full ladder is quoted before withdrawal");
    check(f.gateway.canceled.empty(), "nothing is cancelled before withdrawAfterSeconds");
    check(!lp.metrics()["withdrawing"].get<bool>(), "not yet withdrawing");
    std::cout << "  withdrawal timing checked" << std::endl;
}

void test_withdrawal_pulls_the_whole_book_progressively() {
    Fixture f;
    f.params = {{"referencePrice", 500.00}, {"bidLevels", 5}, {"askLevels", 5},
                {"mode", "withdraw"}, {"withdrawAfterSeconds", 5},
                {"withdrawDurationMs", 2000}};
    LiquidityProvider lp;
    auto c0 = ctxAt(f, 0.0); lp.onStart(c0);
    { f.advance(100); auto c = ctxAt(f, 0.1); lp.onTick(c); }
    check(f.tracker.activeCount() == 10, "ladder seeded");

    // Halfway through the withdrawal window, roughly half the book should be gone.
    f.advance(100); { auto c = ctxAt(f, 6.0); lp.onTick(c); }
    const int midway = f.tracker.activeCount();
    check(midway > 0 && midway < 10,
          "withdrawal is progressive, not a cliff (got " + std::to_string(midway) + " live)");

    // Past the end of the window, everything is gone.
    f.advance(100); { auto c = ctxAt(f, 8.0); lp.onTick(c); }
    check(f.tracker.activeCount() == 0, "the whole book is withdrawn by the end");
    check(lp.metrics()["withdraw_complete"].get<bool>(), "withdrawal reports complete");
    std::cout << "  progressive withdrawal checked" << std::endl;
}

void test_withdrawn_levels_are_not_replenished() {
    Fixture f;
    f.params = {{"referencePrice", 500.00}, {"bidLevels", 5}, {"askLevels", 5},
                {"mode", "withdraw"}, {"withdrawAfterSeconds", 1},
                {"withdrawDurationMs", 500}};
    LiquidityProvider lp;
    auto c0 = ctxAt(f, 0.0); lp.onStart(c0);
    { f.advance(100); auto c = ctxAt(f, 0.1); lp.onTick(c); }
    const size_t afterSeed = f.gateway.submissions.size();

    // Drive well past the end of the withdrawal. The reconcile loop would happily
    // refill every empty level on the next tick, which would undo the whole event.
    for (int i = 0; i < 40; ++i) {
        f.advance(100);
        auto c = ctxAt(f, 2.0 + i * 0.1);
        lp.onTick(c);
    }
    check(f.gateway.submissions.size() == afterSeed,
          "no level is re-quoted after being withdrawn (got " +
              std::to_string(f.gateway.submissions.size() - afterSeed) + " extra)");
    check(f.tracker.activeCount() == 0, "the book stays empty");
    std::cout << "  no replenishment after withdrawal checked" << std::endl;
}

void test_inside_out_pulls_the_touch_first() {
    Fixture f;
    f.params = {{"referencePrice", 500.00}, {"bidLevels", 5}, {"askLevels", 5},
                {"halfSpreadTicks", 1}, {"levelSpacingTicks", 5},
                {"mode", "withdraw"}, {"withdrawAfterSeconds", 1},
                {"withdrawDurationMs", 2000}, {"withdrawOrder", "inside_out"}};
    LiquidityProvider lp;
    auto c0 = ctxAt(f, 0.0); lp.onStart(c0);
    { f.advance(100); auto c = ctxAt(f, 0.1); lp.onTick(c); }
    // 40% through the window: the two innermost levels per side should be gone.
    f.advance(100); { auto c = ctxAt(f, 1.8); lp.onTick(c); }

    double bestBid = 0.0, bestAsk = 1e9;
    for (const auto& s : f.gateway.submissions) {
        if (!f.tracker.isLiveOrder(s.id)) continue;
        if (s.buy) bestBid = std::max(bestBid, s.price);
        else       bestAsk = std::min(bestAsk, s.price);
    }
    // Pulling from the touch outward must WIDEN the spread, which is the whole point
    // of this ordering: depth falls and the market visibly gets worse to trade on.
    check(bestBid > 0.0 && bestBid < 499.99 - 1e-9,
          "inside-out leaves the best bid further from the reference (got " +
              std::to_string(bestBid) + ")");
    check(bestAsk < 1e9 && bestAsk > 500.01 + 1e-9,
          "inside-out leaves the best ask further from the reference (got " +
              std::to_string(bestAsk) + ")");
    std::cout << "  inside-out ordering checked" << std::endl;
}

void test_outside_in_holds_the_touch() {
    Fixture f;
    f.params = {{"referencePrice", 500.00}, {"bidLevels", 5}, {"askLevels", 5},
                {"halfSpreadTicks", 1}, {"levelSpacingTicks", 5},
                {"mode", "withdraw"}, {"withdrawAfterSeconds", 1},
                {"withdrawDurationMs", 2000}, {"withdrawOrder", "outside_in"}};
    LiquidityProvider lp;
    auto c0 = ctxAt(f, 0.0); lp.onStart(c0);
    { f.advance(100); auto c = ctxAt(f, 0.1); lp.onTick(c); }
    f.advance(100); { auto c = ctxAt(f, 1.8); lp.onTick(c); }

    double bestBid = 0.0, bestAsk = 1e9;
    int live = 0;
    for (const auto& s : f.gateway.submissions) {
        if (!f.tracker.isLiveOrder(s.id)) continue;
        ++live;
        if (s.buy) bestBid = std::max(bestBid, s.price);
        else       bestAsk = std::min(bestAsk, s.price);
    }
    check(live < 10, "depth is reduced");
    // The contrast with inside_out: the touch survives, so the spread looks healthy
    // while the book behind it is being emptied.
    check(std::fabs(bestBid - 499.99) < 1e-9, "outside-in keeps the best bid at the touch");
    check(std::fabs(bestAsk - 500.01) < 1e-9, "outside-in keeps the best ask at the touch");
    std::cout << "  outside-in ordering checked" << std::endl;
}

void test_metrics_are_reported() {
    Fixture f;
    f.params = {{"referencePrice", 500.00}, {"bidLevels", 4}, {"askLevels", 4}};
    LiquidityProvider lp;
    auto c1 = f.ctx(); lp.onStart(c1);
    for (int i = 0; i < 5; ++i) { f.advance(100); auto c = f.ctx(); lp.onTick(c); }

    auto m = lp.metrics();
    check(m["desired_levels"] == 8, "metrics report the desired level count");
    check(m["replenishments"] == 8, "metrics count the initial seeding as replenishments");
    check(m["recenters"] == 0, "no recentering occurred");
    check(m["mean_live_levels"].get<double>() > 0.0, "mean live levels is populated");
    check(m["mean_bid_depth"].get<double>() > 0.0, "mean bid depth is populated");
    std::cout << "  metrics checked" << std::endl;
}

}  // namespace

void run_liquidity_provider_tests() {
    std::cout << "--- LiquidityProvider Tests ---" << std::endl;
    test_ladder_geometry();
    test_quantity_variance_is_bounded_and_seed_reproducible();
    test_max_distance_truncates_ladder();
    test_low_reference_price_never_goes_negative();
    test_param_validation();
    test_seeds_full_ladder_on_first_tick();
    test_steady_state_does_not_churn();
    test_replenishes_exactly_once_after_a_fill();
    test_partial_fill_does_not_replenish();
    test_recentering_moves_the_ladder();
    test_rejected_submission_is_retried();
    test_inventory_skew_sign_and_magnitude();
    test_inventory_skew_is_capped_and_disabled_by_default();
    test_ladder_moves_down_as_inventory_grows();
    test_ladder_moves_up_when_short();
    test_no_shift_without_inventory_change();
    test_withdraw_mode_is_accepted_and_validated();
    test_withdrawal_does_not_start_early();
    test_withdrawal_pulls_the_whole_book_progressively();
    test_withdrawn_levels_are_not_replenished();
    test_inside_out_pulls_the_touch_first();
    test_outside_in_holds_the_touch();
    test_metrics_are_reported();

    if (failures != 0) {
        std::cout << failures << " LiquidityProvider assertion(s) FAILED" << std::endl;
        std::exit(1);
    }
    std::cout << "All LiquidityProvider tests passed." << std::endl;
}
