#include "fake_gateway.h"
#include "strategies/parent_executor.h"

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
    RandomSource     rng{17};
    std::ostringstream sink;
    EventLog         log{"test_run", "parent", sink};
    FakeGateway      gateway{tracker};
    nlohmann::json   params;
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

    StrategyContext ctx() {
        return StrategyContext{market, tracker, position, rng, gateway, log,
                               params, now, kTick, 0.0};
    }
    void advance(int ms) { now += std::chrono::milliseconds(ms); }
    void book(int levels = 20, int size = 500) {
        market.bestBid = 499.99; market.bestAsk = 500.01;
        market.bestBidSize = size; market.bestAskSize = size;
        market.askLevels.clear(); market.bidLevels.clear();
        for (int i = 0; i < levels; ++i) {
            market.askLevels.push_back(PriceLevel{500.01 + i * 0.01, size, i + 1});
            market.bidLevels.push_back(PriceLevel{499.99 - i * 0.01, size, i + 1});
        }
    }
    void drive(ParentExecutor& e, int ticks, int stepMs = 50) {
        for (int i = 0; i < ticks && !e.isComplete(); ++i) {
            advance(stepMs); auto c = ctx(); e.onTick(c);
        }
    }
    long long sentTotal() {
        long long n = 0;
        for (const auto& s : gateway.submissions) n += s.size;
        return n;
    }
    // Simulate the engine filling an order, and tell the strategy.
    void fill(ParentExecutor& e, size_t idx, int size, double price) {
        const auto sub = gateway.submissions[idx];
        gateway.fill(sub.id, size);
        Fill f{sub.id, price, size, sub.buy, false, 0};
        auto c = ctx(); e.onFill(c, f);
    }
};

void test_param_validation() {
    auto rejects = [](nlohmann::json j, const std::string& why) {
        try {
            ParentParams::fromJson(j);
            std::cout << "  [FAIL] should have rejected: " << why << std::endl;
            ++failures;
        } catch (const std::runtime_error&) {}
    };
    rejects({{"parentQuantity", 1000}}, "neither limitPrice nor maxLevels");
    rejects({{"parentQuantity", 1000}, {"maxLevels", 5}, {"mode", "vwap"}}, "unknown mode");
    rejects({{"maxLevels", 5}}, "missing parentQuantity");
    rejects({{"parentQuantity", 1000}, {"maxLevels", 5}, {"mode", "sliced"}},
            "sliced without sliceQuantity");
    rejects({{"parentQuantity", 1000}, {"maxLevels", 5},
             {"mode", "participation"}, {"participationRate", 0.0}}, "zero participation rate");
    rejects({{"parentQuantity", 1000}, {"maxLevels", 5},
             {"mode", "participation"}, {"participationRate", 1.5}}, "participation above 1");
    std::cout << "  parameter validation checked" << std::endl;
}

// The Phase 6 sweep vocabulary must keep resolving, or six existing scenarios break.
void test_legacy_sweep_params_still_work() {
    auto p = ParentParams::fromJson(
        {{"side", "BUY"}, {"targetQuantity", 5000}, {"maxLevels", 10}});
    check(p.parentQuantity == 5000, "legacy targetQuantity maps to parentQuantity");
    check(p.mode == "immediate", "a legacy sweep without children is mode immediate");

    auto q = ParentParams::fromJson(
        {{"targetQuantity", 2500}, {"maxLevels", 10},
         {"childOrders", true}, {"childQuantity", 500}, {"childDelayMs", 200}});
    check(q.mode == "sliced", "legacy childOrders maps to mode sliced");
    check(q.sliceQuantity == 500, "legacy childQuantity maps to sliceQuantity");
    check(q.sliceIntervalMs == 200, "legacy childDelayMs maps to sliceIntervalMs");
    std::cout << "  legacy sweep parameters checked" << std::endl;
}

void test_immediate_sends_everything_at_once() {
    Fixture f; f.book();
    f.params = {{"parentQuantity", 3000}, {"maxLevels", 10}, {"mode", "immediate"}};
    ParentExecutor e;
    auto c = f.ctx(); e.onStart(c);
    f.drive(e, 3);
    check(f.gateway.submissions.size() == 1, "one order carries the whole parent");
    check(f.gateway.submissions[0].size == 3000, "order is the full parent quantity");
    std::cout << "  immediate mode checked" << std::endl;
}

void test_sliced_respects_size_and_interval() {
    Fixture f; f.book();
    f.params = {{"parentQuantity", 2000}, {"maxLevels", 10}, {"mode", "sliced"},
                {"sliceQuantity", 500}, {"sliceIntervalMs", 200}};
    ParentExecutor e;
    auto c = f.ctx(); e.onStart(c);

    f.drive(e, 4, 50);   // 200ms => at most 2 slices
    check(f.gateway.submissions.size() <= 2, "interval throttles slicing");
    f.drive(e, 40, 50);
    check(f.sentTotal() == 2000, "sends exactly the parent quantity across slices");
    check(f.gateway.submissions.size() == 4, "2000 in 500-share slices is four orders");
    std::cout << "  sliced mode checked" << std::endl;
}

void test_participation_tracks_market_volume() {
    Fixture f; f.book();
    f.params = {{"parentQuantity", 10000}, {"maxLevels", 10}, {"mode", "participation"},
                {"participationRate", 0.2}, {"minSliceQuantity", 10},
                {"sliceIntervalMs", 100}};
    ParentExecutor e;
    auto c = f.ctx(); e.onStart(c);
    f.drive(e, 2);          // arms, market volume still 0 => only the floor
    const long long quiet = f.sentTotal();
    check(quiet <= 20, "a quiet market sends only the minimum slice (got " +
                           std::to_string(quiet) + ")");

    // The market prints 5000 shares; at 20% we are entitled to ~1000.
    f.market.observedVolume += 5000;
    f.drive(e, 6, 100);
    const long long busy = f.sentTotal();
    check(busy >= 800 && busy <= 1300,
          "sends about 20% of observed market volume (got " + std::to_string(busy) + ")");
    std::cout << "  participation mode checked" << std::endl;
}

void test_participation_never_exceeds_the_parent() {
    Fixture f; f.book();
    f.params = {{"parentQuantity", 1000}, {"maxLevels", 10}, {"mode", "participation"},
                {"participationRate", 0.5}, {"minSliceQuantity", 10},
                {"sliceIntervalMs", 50}};
    ParentExecutor e;
    auto c = f.ctx(); e.onStart(c);
    f.market.observedVolume += 100000;   // enormous market volume
    f.drive(e, 40, 50);
    check(f.sentTotal() <= 1000, "never sends more than the parent quantity (got " +
                                     std::to_string(f.sentTotal()) + ")");
    std::cout << "  participation budget checked" << std::endl;
}

void test_passive_first_rests_then_crosses_when_behind() {
    Fixture f; f.book();
    f.params = {{"parentQuantity", 1000}, {"limitPrice", 500.50},
                {"mode", "passive_first"}, {"horizonSeconds", 10.0},
                {"catchupThreshold", 0.15}, {"sliceIntervalMs", 100}};
    ParentExecutor e;
    auto c = f.ctx(); e.onStart(c);

    // Early on it is on schedule, so it should rest at the near touch rather than pay
    // the spread.
    f.drive(e, 3, 100);
    check(!f.gateway.submissions.empty(), "posts something early");
    check(f.gateway.submissions[0].price <= 499.99 + 1e-9,
          "starts passive at the near touch (got " +
              std::to_string(f.gateway.submissions[0].price) + ")");

    // Nothing fills, so it falls behind schedule and must cross.
    f.drive(e, 40, 100);
    bool crossed = false;
    for (const auto& s : f.gateway.submissions)
        if (s.price >= 500.01 - 1e-9) crossed = true;
    check(crossed, "crosses once it falls behind schedule");
    check(e.metrics()["catchup_events"].get<int>() > 0, "catch-up events are reported");
    std::cout << "  passive_first mode checked" << std::endl;
}

void test_passive_first_does_not_overshoot_when_filling() {
    Fixture f; f.book();
    f.params = {{"parentQuantity", 1000}, {"limitPrice", 500.50},
                {"mode", "passive_first"}, {"horizonSeconds", 10.0},
                {"sliceIntervalMs", 100}};
    ParentExecutor e;
    auto c = f.ctx(); e.onStart(c);
    f.drive(e, 3, 100);

    // Fill the whole parent on the first resting order. Repeatedly cancelling and
    // re-posting must not let filled quantity exceed the parent.
    f.fill(e, 0, 1000, 499.99);
    f.drive(e, 20, 100);
    check(e.isComplete(), "completes once the parent is filled");
    check(e.metrics()["filled_quantity"].get<long long>() == 1000,
          "does not overfill the parent");
    check(f.tracker.activeCount() == 0, "leaves nothing working");
    std::cout << "  passive_first completion checked" << std::endl;
}

void test_completes_and_cancels_residual() {
    Fixture f; f.book();
    f.params = {{"parentQuantity", 2000}, {"maxLevels", 10}, {"mode", "immediate"},
                {"settleMs", 200}};
    ParentExecutor e;
    auto c = f.ctx(); e.onStart(c);
    f.drive(e, 3);
    f.fill(e, 0, 700, 500.02);      // only part trades
    f.drive(e, 20, 50);
    check(e.isComplete(), "completes after the settle window");
    check(!f.gateway.canceled.empty(), "unfilled residual is cancelled");
    check(f.tracker.activeCount() == 0, "nothing left resting");
    std::cout << "  settle and residual checked" << std::endl;
}

void test_vwap_and_slippage_reported() {
    Fixture f; f.book();
    f.params = {{"parentQuantity", 1000}, {"maxLevels", 10}, {"mode", "immediate"}};
    ParentExecutor e;
    auto c = f.ctx(); e.onStart(c);
    f.drive(e, 3);
    f.fill(e, 0, 500, 500.01);
    f.fill(e, 0, 500, 500.05);
    f.drive(e, 5);

    auto m = e.metrics();
    check(std::fabs(m["vwap"].get<double>() - 500.03) < 1e-9, "VWAP averages the fills");
    check(m["slippage_bps"].get<double>() > 0.0,
          "buying above the arrival mid is positive slippage");
    check(m["completion_seconds"].get<double>() >= 0.0, "completion time reported");
    std::cout << "  VWAP and slippage checked" << std::endl;
}

// --- ported from the Phase 6 sweep tests, which this class supersedes -----------

void test_derives_limit_price_from_max_levels() {
    Fixture f; f.book(10, 500);
    f.params = {{"parentQuantity", 2000}, {"maxLevels", 4}, {"mode", "immediate"}};
    ParentExecutor e;
    auto c = f.ctx(); e.onStart(c);
    f.drive(e, 3);
    // Four levels from 500.01 in one-tick steps => the fourth is 500.04.
    check(std::fabs(f.gateway.submissions[0].price - 500.04) < 1e-9,
          "limit is the Nth level of the book at arrival (got " +
              std::to_string(f.gateway.submissions[0].price) + ")");
    std::cout << "  limit derivation checked" << std::endl;
}

void test_slices_send_exact_remainder() {
    Fixture f; f.book();
    f.params = {{"parentQuantity", 1750}, {"maxLevels", 10}, {"mode", "sliced"},
                {"sliceQuantity", 500}, {"sliceIntervalMs", 10}};
    ParentExecutor e;
    auto c = f.ctx(); e.onStart(c);
    f.drive(e, 60, 20);
    check(f.sentTotal() == 1750,
          "sends exactly the parent, not a rounded-up multiple of the slice");
    check(f.gateway.submissions.back().size == 250, "final slice is the remainder");
    std::cout << "  slice remainder checked" << std::endl;
}

void test_sell_side_slippage_sign() {
    Fixture f; f.book();
    f.params = {{"side", "SELL"}, {"parentQuantity", 500}, {"maxLevels", 3},
                {"mode", "immediate"}};
    ParentExecutor e;
    auto c = f.ctx(); e.onStart(c);
    f.drive(e, 3);
    check(!f.gateway.submissions[0].buy, "sell parent sends a sell order");
    f.fill(e, 0, 500, 499.97);
    f.drive(e, 5);
    // Selling below the arrival mid is also "worse", so slippage stays positive on
    // both sides -- otherwise buy and sell executions could not be compared.
    check(e.metrics()["slippage_bps"].get<double>() > 0.0,
          "selling below the arrival mid reports positive slippage");
    std::cout << "  sell-side slippage sign checked" << std::endl;
}

void test_gives_up_after_max_duration() {
    Fixture f; f.book();
    f.params = {{"parentQuantity", 100000}, {"maxLevels", 10}, {"mode", "sliced"},
                {"sliceQuantity", 100}, {"sliceIntervalMs", 100}, {"maxDurationMs", 500}};
    ParentExecutor e;
    auto c = f.ctx(); e.onStart(c);
    f.drive(e, 100, 50);
    check(e.isComplete(), "a parent that cannot complete still terminates");
    check(f.tracker.activeCount() == 0, "leaves nothing working on timeout");
    std::cout << "  max duration checked" << std::endl;
}

void test_waits_for_a_market() {
    Fixture f;   // no book
    f.params = {{"parentQuantity", 1000}, {"maxLevels", 5}, {"maxDurationMs", 100000}};
    ParentExecutor e;
    auto c = f.ctx(); e.onStart(c);
    f.drive(e, 10);
    check(f.gateway.submissions.empty(), "does not fire into an empty book");
    f.book();
    f.drive(e, 3);
    check(!f.gateway.submissions.empty(), "fires once a book appears");
    std::cout << "  arming guard checked" << std::endl;
}

}  // namespace

void run_parent_executor_tests() {
    std::cout << "--- ParentExecutor Tests ---" << std::endl;
    test_param_validation();
    test_legacy_sweep_params_still_work();
    test_immediate_sends_everything_at_once();
    test_sliced_respects_size_and_interval();
    test_participation_tracks_market_volume();
    test_participation_never_exceeds_the_parent();
    test_passive_first_rests_then_crosses_when_behind();
    test_passive_first_does_not_overshoot_when_filling();
    test_completes_and_cancels_residual();
    test_vwap_and_slippage_reported();
    test_derives_limit_price_from_max_levels();
    test_slices_send_exact_remainder();
    test_sell_side_slippage_sign();
    test_gives_up_after_max_duration();
    test_waits_for_a_market();
    if (failures != 0) {
        std::cout << failures << " ParentExecutor assertion(s) FAILED" << std::endl;
        std::exit(1);
    }
    std::cout << "All ParentExecutor tests passed." << std::endl;
}
