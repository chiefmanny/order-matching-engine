#include "fake_gateway.h"
#include "strategies/noise_trader.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <sstream>

namespace {

constexpr double kTick = 0.01;
int failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) { std::cout << "  [FAIL] " << what << std::endl; ++failures; }
}

struct Fixture {
    MarketState      market;
    OrderTracker     tracker;
    PositionTracker  position;
    RandomSource     rng{99};
    std::ostringstream sink;
    EventLog         log{"test_run", "noise", sink};
    FakeGateway      gateway{tracker};
    nlohmann::json   params;
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

    StrategyContext ctx() {
        return StrategyContext{market, tracker, position, rng, gateway, log,
                               params, now, kTick, 0.0};
    }
    void advance(int ms) { now += std::chrono::milliseconds(ms); }

    void quoteTwoSided(double bid = 499.99, double ask = 500.01) {
        market.bestBid = bid; market.bestAsk = ask;
        market.bestBidSize = 500; market.bestAskSize = 500;
    }
    // Runs `ticks` decision ticks at `stepMs` each.
    void drive(NoiseTrader& nt, int ticks, int stepMs = 50) {
        for (int i = 0; i < ticks; ++i) {
            advance(stepMs);
            auto c = ctx();
            nt.onTick(c);
        }
    }
};

void test_respects_arrival_interval() {
    Fixture f;
    f.quoteTwoSided();
    f.params = {{"orderIntervalMs", 300}, {"intervalJitterMs", 0},
                {"aggressiveProbability", 0.0}, {"cancelProbability", 0.0},
                {"maxOpenOrders", 1000}};
    NoiseTrader nt;
    auto c = f.ctx(); nt.onStart(c);

    // Ticking every 50ms for 3s => ~10 actions at a 300ms interval, not 60.
    f.drive(nt, 60, 50);
    const int sent = static_cast<int>(f.gateway.submissions.size());
    check(sent >= 8 && sent <= 12,
          "arrival interval governs order count, not the tick cadence (got " +
              std::to_string(sent) + ", expected ~10)");
    std::cout << "  arrival interval checked" << std::endl;
}

void test_aggressive_probability_controls_crossing() {
    auto run = [](double aggressive) {
        Fixture f;
        f.quoteTwoSided();
        f.params = {{"orderIntervalMs", 50}, {"intervalJitterMs", 0},
                    {"aggressiveProbability", aggressive}, {"cancelProbability", 0.0},
                    {"maxOpenOrders", 100000}};
        NoiseTrader nt;
        auto c = f.ctx(); nt.onStart(c);
        f.drive(nt, 400, 50);

        // An aggressive buy sits at/above the best ask; a passive buy at/below the bid.
        int crossing = 0;
        for (const auto& s : f.gateway.submissions) {
            const bool crosses = s.buy ? s.price >= 500.01 - 1e-9 : s.price <= 499.99 + 1e-9;
            if (crosses) ++crossing;
        }
        return std::make_pair(crossing, static_cast<int>(f.gateway.submissions.size()));
    };

    auto none = run(0.0);
    check(none.first == 0, "aggressiveProbability=0 never crosses the spread");
    check(none.second > 100, "orders were still sent when fully passive");

    auto all = run(1.0);
    check(all.first == all.second, "aggressiveProbability=1 always crosses the spread");

    auto some = run(0.25);
    const double share = static_cast<double>(some.first) / some.second;
    check(share > 0.12 && share < 0.40,
          "aggressiveProbability=0.25 crosses roughly a quarter of the time (got " +
              std::to_string(share) + ")");
    std::cout << "  aggressiveness checked" << std::endl;
}

void test_passive_orders_never_cross_the_spread() {
    Fixture f;
    f.quoteTwoSided(499.90, 500.10);   // a deliberately wide, 20-tick spread
    f.params = {{"orderIntervalMs", 50}, {"intervalJitterMs", 0},
                {"aggressiveProbability", 0.0}, {"cancelProbability", 0.0},
                {"passivePlacementTicks", 5}, {"maxOpenOrders", 100000}};
    NoiseTrader nt;
    auto c = f.ctx(); nt.onStart(c);
    f.drive(nt, 300, 50);

    // Noise must not tighten the spread: that is the market maker's role, and letting
    // background flow do it would blur the effect Phase 8 sets out to measure.
    bool allOutside = true, allOnGrid = true;
    for (const auto& s : f.gateway.submissions) {
        if (s.buy && s.price > 499.90 + 1e-9) allOutside = false;
        if (!s.buy && s.price < 500.10 - 1e-9) allOutside = false;
        const double ticks = s.price / kTick;
        if (std::fabs(ticks - std::llround(ticks)) > 1e-6) allOnGrid = false;
    }
    check(allOutside, "passive orders never post inside the spread");
    check(allOnGrid, "every price lands on the tick grid");
    std::cout << "  passive placement checked" << std::endl;
}

void test_bootstraps_when_book_is_empty() {
    Fixture f;   // market deliberately left invalid (no quotes)
    f.params = {{"orderIntervalMs", 50}, {"intervalJitterMs", 0},
                {"referencePrice", 500.00}, {"maxOpenOrders", 100000}};
    NoiseTrader nt;
    auto c = f.ctx(); nt.onStart(c);
    f.drive(nt, 100, 50);

    check(!f.gateway.submissions.empty(),
          "with a referencePrice, noise can seed a market with no book present");
    bool sane = true;
    for (const auto& s : f.gateway.submissions) {
        if (s.price <= 0.0 || std::fabs(s.price - 500.00) > 0.20) sane = false;
    }
    check(sane, "bootstrap orders cluster around the reference price");
    std::cout << "  bootstrap checked" << std::endl;
}

void test_idles_when_no_market_and_no_reference() {
    Fixture f;   // no quotes AND no referencePrice
    f.params = {{"orderIntervalMs", 50}, {"intervalJitterMs", 0}, {"maxOpenOrders", 100}};
    NoiseTrader nt;
    auto c = f.ctx(); nt.onStart(c);
    f.drive(nt, 60, 50);

    check(f.gateway.submissions.empty(),
          "no market and no reference => no orders, rather than nonsense prices");
    check(nt.metrics()["skipped_no_market"].get<long long>() > 0,
          "idling is reported in metrics rather than being silent");
    std::cout << "  idle guard checked" << std::endl;
}

void test_open_order_cap_is_enforced() {
    Fixture f;
    f.quoteTwoSided();
    f.params = {{"orderIntervalMs", 50}, {"intervalJitterMs", 0},
                {"aggressiveProbability", 0.0}, {"cancelProbability", 0.0},
                {"maxOpenOrders", 5}};
    NoiseTrader nt;
    auto c = f.ctx(); nt.onStart(c);
    f.drive(nt, 200, 50);

    // Nothing fills in this fixture, so every order rests; the cap must bind.
    check(f.tracker.activeCount() <= 5, "never exceeds maxOpenOrders resting orders");
    check(nt.metrics()["skipped_at_open_order_cap"].get<long long>() > 0,
          "hitting the cap is reported");
    std::cout << "  open order cap checked" << std::endl;
}

void test_cancels_only_its_own_orders() {
    Fixture f;
    f.quoteTwoSided();
    f.params = {{"orderIntervalMs", 50}, {"intervalJitterMs", 0},
                {"aggressiveProbability", 0.0}, {"cancelProbability", 0.6},
                {"maxOpenOrders", 100000}};
    NoiseTrader nt;
    auto c = f.ctx(); nt.onStart(c);
    f.drive(nt, 300, 50);

    check(!f.gateway.canceled.empty(), "cancelProbability produces cancels");
    check(f.gateway.ownershipViolations.empty(),
          "never attempts to cancel an order it does not own");
    for (unsigned int id : f.gateway.canceled) {
        check(f.tracker.owns(id), "every cancelled id belongs to this participant");
    }
    std::cout << "  cancellation checked" << std::endl;
}

void test_quantities_stay_within_bounds() {
    Fixture f;
    f.quoteTwoSided();
    f.params = {{"orderIntervalMs", 50}, {"intervalJitterMs", 0},
                {"meanQuantity", 100}, {"quantityVariance", 40},
                {"cancelProbability", 0.0}, {"maxOpenOrders", 100000}};
    NoiseTrader nt;
    auto c = f.ctx(); nt.onStart(c);
    f.drive(nt, 200, 50);

    bool inBounds = true, varied = false;
    for (const auto& s : f.gateway.submissions) {
        if (s.size < 60 || s.size > 140) inBounds = false;
        if (s.size != 100) varied = true;
    }
    check(inBounds, "order sizes stay within meanQuantity +/- variance");
    check(varied, "order sizes actually vary");
    std::cout << "  sizing checked" << std::endl;
}

void test_seed_reproducibility() {
    auto run = [](unsigned int seed) {
        Fixture f;
        f.rng = RandomSource(seed);
        f.quoteTwoSided();
        f.params = {{"orderIntervalMs", 100}, {"intervalJitterMs", 40},
                    {"aggressiveProbability", 0.3}, {"cancelProbability", 0.2},
                    {"maxOpenOrders", 100000}};
        NoiseTrader nt;
        auto c = f.ctx(); nt.onStart(c);
        f.drive(nt, 200, 50);
        std::string trace;
        for (const auto& s : f.gateway.submissions) {
            trace += (s.buy ? "B" : "S") + std::to_string(s.size) + "@" +
                     std::to_string(static_cast<long long>(s.price * 100)) + ";";
        }
        return trace;
    };

    // Reproducibility is what makes an experiment re-runnable, so it is a property
    // of the strategy, not just of the RNG.
    check(run(1234) == run(1234), "same seed reproduces an identical order sequence");
    check(run(1234) != run(5678), "a different seed produces a different sequence");
    std::cout << "  reproducibility checked" << std::endl;
}

void test_param_validation() {
    auto rejects = [](nlohmann::json j, const std::string& why) {
        try {
            NoiseParams::fromJson(j);
            std::cout << "  [FAIL] should have rejected: " << why << std::endl;
            ++failures;
        } catch (const std::runtime_error&) {}
    };
    rejects({{"orderIntervalMs", 0}}, "zero arrival interval");
    rejects({{"orderIntervalMs", 100}, {"intervalJitterMs", 100}}, "jitter >= interval");
    rejects({{"meanQuantity", 0}}, "zero quantity");
    rejects({{"meanQuantity", 50}, {"quantityVariance", 50}}, "variance >= mean");
    rejects({{"aggressiveProbability", 1.5}}, "probability above 1");
    rejects({{"cancelProbability", -0.1}}, "negative probability");
    rejects({{"maxOpenOrders", 0}}, "zero open-order cap");

    NoiseParams ok = NoiseParams::fromJson({{"orderIntervalMs", 250}});
    check(ok.orderIntervalMs == 250, "valid config parses");
    check(ok.aggressiveProbability == 0.20, "unspecified fields take defaults");
    std::cout << "  parameter validation checked" << std::endl;
}

}  // namespace

void run_noise_trader_tests() {
    std::cout << "--- NoiseTrader Tests ---" << std::endl;
    test_respects_arrival_interval();
    test_aggressive_probability_controls_crossing();
    test_passive_orders_never_cross_the_spread();
    test_bootstraps_when_book_is_empty();
    test_idles_when_no_market_and_no_reference();
    test_open_order_cap_is_enforced();
    test_cancels_only_its_own_orders();
    test_quantities_stay_within_bounds();
    test_seed_reproducibility();
    test_param_validation();

    if (failures != 0) {
        std::cout << failures << " NoiseTrader assertion(s) FAILED" << std::endl;
        std::exit(1);
    }
    std::cout << "All NoiseTrader tests passed." << std::endl;
}
