#include "fake_gateway.h"
#include "strategies/sweep.h"

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
    RandomSource     rng{5};
    std::ostringstream sink;
    EventLog         log{"test_run", "sweep", sink};
    FakeGateway      gateway{tracker};
    nlohmann::json   params;
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

    StrategyContext ctx() {
        return StrategyContext{market, tracker, position, rng, gateway, log,
                               params, now, kTick, 0.0};
    }
    void advance(int ms) { now += std::chrono::milliseconds(ms); }

    // A book with `n` ask levels of `size` each, one tick apart above 500.00.
    void bookWithAsks(int n, int size) {
        market.bestBid = 499.99; market.bestBidSize = 500;
        market.bestAsk = 500.01; market.bestAskSize = size;
        market.askLevels.clear();
        for (int i = 0; i < n; ++i)
            market.askLevels.push_back(PriceLevel{500.01 + i * 0.01, size, i + 1});
        market.bidLevels.clear();
        market.bidLevels.push_back(PriceLevel{499.99, 500, 1});
    }
    void drive(Sweep& s, int ticks, int stepMs = 50) {
        for (int i = 0; i < ticks && !s.isComplete(); ++i) {
            advance(stepMs);
            auto c = ctx();
            s.onTick(c);
        }
    }
};

void test_param_validation() {
    auto rejects = [](nlohmann::json j, const std::string& why) {
        try {
            SweepParams::fromJson(j);
            std::cout << "  [FAIL] should have rejected: " << why << std::endl;
            ++failures;
        } catch (const std::runtime_error&) {}
    };
    rejects({{"targetQuantity", 100}}, "neither limitPrice nor maxLevels");
    rejects({{"targetQuantity", 100}, {"limitPrice", 500.0}, {"maxLevels", 5}},
            "both limitPrice and maxLevels is ambiguous");
    rejects({{"maxLevels", 5}}, "missing targetQuantity");
    rejects({{"targetQuantity", -5}, {"maxLevels", 3}}, "negative target");
    rejects({{"targetQuantity", 100}, {"maxLevels", 3}, {"side", "SIDEWAYS"}}, "bad side");
    rejects({{"targetQuantity", 100}, {"maxLevels", 3}, {"childOrders", true}},
            "childOrders without childQuantity");

    auto ok = SweepParams::fromJson({{"targetQuantity", 5000}, {"maxLevels", 8}});
    check(ok.isBuy(), "defaults to BUY");
    check(ok.targetQuantity == 5000, "target parsed");
    std::cout << "  parameter validation checked" << std::endl;
}

void test_derives_limit_price_from_max_levels() {
    Fixture f;
    f.bookWithAsks(10, 500);
    f.params = {{"side", "BUY"}, {"targetQuantity", 2000}, {"maxLevels", 4}};
    Sweep s;
    auto c = f.ctx(); s.onStart(c);
    f.drive(s, 3);

    // 4 levels starting at 500.01, one tick apart => the 4th is 500.04.
    check(!f.gateway.submissions.empty(), "sweep submitted");
    for (const auto& sub : f.gateway.submissions)
        check(std::fabs(sub.price - 500.04) < 1e-9,
              "limit is the 4th ask level (got " + std::to_string(sub.price) + ")");
    auto m = s.metrics();
    check(std::fabs(m["limit_price"].get<double>() - 500.04) < 1e-9, "limit recorded");
    check(m["arrival_depth_within_limit"].get<long long>() == 2000,
          "arrival depth within the limit is 4 x 500");
    check(m["arrival_levels_within_limit"].get<int>() == 4, "4 levels within the limit");
    std::cout << "  limit derivation checked" << std::endl;
}

void test_never_sends_more_than_target() {
    Fixture f;
    f.bookWithAsks(20, 500);
    f.params = {{"targetQuantity", 1750}, {"maxLevels", 10},
                {"childOrders", true}, {"childQuantity", 500}, {"childDelayMs", 10}};
    Sweep s;
    auto c = f.ctx(); s.onStart(c);
    f.drive(s, 200, 20);

    long long sent = 0;
    for (const auto& sub : f.gateway.submissions) sent += sub.size;
    check(sent == 1750, "sent exactly the target, not a rounded-up multiple of the child size");
    check(s.metrics()["sent_quantity"].get<long long>() == 1750, "sent quantity recorded");
    // The last child must be the 250-share remainder.
    check(f.gateway.submissions.back().size == 250, "final child is the remainder");
    std::cout << "  target quantity invariant checked" << std::endl;
}

void test_child_slicing_respects_delay() {
    Fixture f;
    f.bookWithAsks(20, 500);
    f.params = {{"targetQuantity", 2000}, {"maxLevels", 10},
                {"childOrders", true}, {"childQuantity", 500}, {"childDelayMs", 200}};
    Sweep s;
    auto c = f.ctx(); s.onStart(c);

    f.drive(s, 4, 50);   // 200ms elapsed => at most 2 children so far
    check(f.gateway.submissions.size() <= 2,
          "child delay throttles submission (got " +
              std::to_string(f.gateway.submissions.size()) + ")");
    f.drive(s, 40, 50);
    check(f.gateway.submissions.size() == 4, "all four children eventually sent");
    std::cout << "  child slicing checked" << std::endl;
}

void test_single_order_when_not_slicing() {
    Fixture f;
    f.bookWithAsks(20, 500);
    f.params = {{"targetQuantity", 3000}, {"maxLevels", 10}};
    Sweep s;
    auto c = f.ctx(); s.onStart(c);
    f.drive(s, 5);
    check(f.gateway.submissions.size() == 1, "one order carries the whole quantity");
    check(f.gateway.submissions[0].size == 3000, "order is the full target");
    std::cout << "  unsliced sweep checked" << std::endl;
}

void test_cancels_unfilled_residual() {
    Fixture f;
    f.bookWithAsks(20, 500);
    f.params = {{"targetQuantity", 2000}, {"maxLevels", 10}, {"settleMs", 200}};
    Sweep s;
    auto c = f.ctx(); s.onStart(c);
    f.drive(s, 3);
    check(f.gateway.submissions.size() == 1, "order sent");

    // Only part of it trades; the rest would otherwise rest inside the book as new
    // liquidity at an aggressive price and contaminate post-sweep measurements.
    const auto sub = f.gateway.submissions[0];
    f.gateway.fill(sub.id, 700);
    Fill fill{sub.id, sub.price, 700, true, false, 0};
    auto c2 = f.ctx(); s.onFill(c2, fill);

    f.drive(s, 20, 50);
    check(s.isComplete(), "sweep completes after the settle window");
    check(!f.gateway.canceled.empty(), "unfilled residual is cancelled");
    check(f.tracker.activeCount() == 0, "no sweep order is left resting");
    check(s.metrics()["residual_cancelled"].get<bool>(), "residual cancellation recorded");
    std::cout << "  residual cancellation checked" << std::endl;
}

void test_completes_when_fully_filled() {
    Fixture f;
    f.bookWithAsks(20, 500);
    f.params = {{"targetQuantity", 1000}, {"maxLevels", 10}};
    Sweep s;
    auto c = f.ctx(); s.onStart(c);
    f.drive(s, 3);

    const auto sub = f.gateway.submissions[0];
    f.gateway.fill(sub.id, 1000);
    Fill fill{sub.id, 500.03, 1000, true, false, 0};
    auto c2 = f.ctx(); s.onFill(c2, fill);
    f.drive(s, 5);

    check(s.isComplete(), "sweep completes as soon as the target is filled");
    auto m = s.metrics();
    check(m["filled_quantity"].get<long long>() == 1000, "filled quantity recorded");
    check(std::fabs(m["fill_completion"].get<double>() - 1.0) < 1e-9, "100% completion");
    std::cout << "  completion checked" << std::endl;
}

void test_vwap_and_slippage() {
    Fixture f;
    f.bookWithAsks(20, 500);
    f.params = {{"targetQuantity", 1000}, {"maxLevels", 10}};
    Sweep s;
    auto c = f.ctx(); s.onStart(c);
    f.drive(s, 3);
    const auto sub = f.gateway.submissions[0];

    // Walk the book: 500 @ 500.01 then 500 @ 500.05 => VWAP 500.03.
    for (auto pair : {std::make_pair(500.01, 500), std::make_pair(500.05, 500)}) {
        f.gateway.fill(sub.id, pair.second);
        Fill fill{sub.id, pair.first, pair.second, true, false, 0};
        auto cc = f.ctx(); s.onFill(cc, fill);
    }
    f.drive(s, 5);

    auto m = s.metrics();
    check(std::fabs(m["vwap"].get<double>() - 500.03) < 1e-9,
          "VWAP averages the levels consumed (got " +
              std::to_string(m["vwap"].get<double>()) + ")");
    // Arrival mid = (499.99 + 500.01)/2 = 500.00; paying 500.03 is 0.6 bps worse.
    const double slip = m["slippage_bps"].get<double>();
    check(std::fabs(slip - 0.6) < 0.05,
          "slippage is positive and measured against the arrival mid (got " +
              std::to_string(slip) + ")");
    std::cout << "  VWAP and slippage checked" << std::endl;
}

void test_sell_side_slippage_sign() {
    Fixture f;
    f.bookWithAsks(10, 500);
    f.market.bidLevels.clear();
    for (int i = 0; i < 10; ++i)
        f.market.bidLevels.push_back(PriceLevel{499.99 - i * 0.01, 500, i + 1});
    f.params = {{"side", "SELL"}, {"targetQuantity", 500}, {"maxLevels", 3}};
    Sweep s;
    auto c = f.ctx(); s.onStart(c);
    f.drive(s, 3);
    check(!f.gateway.submissions.empty(), "sell sweep submitted");
    check(!f.gateway.submissions[0].buy, "sell sweep sends a sell order");

    const auto sub = f.gateway.submissions[0];
    f.gateway.fill(sub.id, 500);
    Fill fill{sub.id, 499.97, 500, false, false, 0};
    auto c2 = f.ctx(); s.onFill(c2, fill);
    f.drive(s, 5);

    // Selling below the arrival mid is also "worse", so slippage must stay positive.
    const double slip = s.metrics()["slippage_bps"].get<double>();
    check(slip > 0.0, "selling below the arrival mid reports positive slippage (got " +
                          std::to_string(slip) + ")");
    std::cout << "  sell-side slippage sign checked" << std::endl;
}

void test_waits_for_market_before_arming() {
    Fixture f;   // no market at all
    f.params = {{"targetQuantity", 1000}, {"maxLevels", 5}, {"maxDurationMs", 100000}};
    Sweep s;
    auto c = f.ctx(); s.onStart(c);
    f.drive(s, 10, 50);
    check(f.gateway.submissions.empty(), "does not fire into an empty book");
    check(!s.metrics()["armed"].get<bool>(), "not armed without a usable book");

    f.bookWithAsks(10, 500);
    f.drive(s, 5, 50);
    check(!f.gateway.submissions.empty(), "fires once a book appears");
    std::cout << "  arming guard checked" << std::endl;
}

void test_gives_up_after_max_duration() {
    Fixture f;
    f.bookWithAsks(10, 500);
    f.params = {{"targetQuantity", 100000}, {"maxLevels", 10}, {"maxDurationMs", 500},
                {"childOrders", true}, {"childQuantity", 100}, {"childDelayMs", 100}};
    Sweep s;
    auto c = f.ctx(); s.onStart(c);
    f.drive(s, 100, 50);
    check(s.isComplete(), "a sweep that cannot complete still terminates");
    check(f.tracker.activeCount() == 0, "leaves nothing resting on timeout");
    std::cout << "  max duration checked" << std::endl;
}

}  // namespace

void run_sweep_tests() {
    std::cout << "--- Sweep Tests ---" << std::endl;
    test_param_validation();
    test_derives_limit_price_from_max_levels();
    test_never_sends_more_than_target();
    test_child_slicing_respects_delay();
    test_single_order_when_not_slicing();
    test_cancels_unfilled_residual();
    test_completes_when_fully_filled();
    test_vwap_and_slippage();
    test_sell_side_slippage_sign();
    test_waits_for_market_before_arming();
    test_gives_up_after_max_duration();
    if (failures != 0) {
        std::cout << failures << " Sweep assertion(s) FAILED" << std::endl;
        std::exit(1);
    }
    std::cout << "All Sweep tests passed." << std::endl;
}
