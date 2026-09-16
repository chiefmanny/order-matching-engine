#include "fake_gateway.h"
#include "strategies/directional_trader.h"

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
    RandomSource     rng{3};
    std::ostringstream sink;
    EventLog         log{"test_run", "dir", sink};
    FakeGateway      gateway{tracker};
    nlohmann::json   params;
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    double elapsed = 0.0;

    StrategyContext ctx() {
        return StrategyContext{market, tracker, position, rng, gateway, log,
                               params, now, kTick, elapsed};
    }
    void advance(int ms) { now += std::chrono::milliseconds(ms); elapsed += ms / 1000.0; }
    void quote(double bid = 499.90, double ask = 500.10) {
        market.bestBid = bid; market.bestAsk = ask;
        market.bestBidSize = 500; market.bestAskSize = 500;
    }
    void drive(DirectionalTrader& d, int ticks, int stepMs = 100) {
        for (int i = 0; i < ticks && !d.isComplete(); ++i) {
            advance(stepMs); auto c = ctx(); d.onTick(c);
        }
    }
};

void test_param_validation() {
    auto rejects = [](nlohmann::json j, const std::string& why) {
        try {
            DirectionalParams::fromJson(j);
            std::cout << "  [FAIL] should have rejected: " << why << std::endl;
            ++failures;
        } catch (const std::runtime_error&) {}
    };
    rejects({{"side", "UP"}}, "bad side");
    rejects({{"orderSize", 0}}, "zero order size");
    rejects({{"arrivalIntervalMs", 0}}, "zero arrival interval");
    rejects({{"arrivalIntervalMs", 100}, {"intervalJitterMs", 100}}, "jitter >= interval");
    rejects({{"aggressiveness", 1.5}}, "aggressiveness above 1");
    rejects({{"aggressiveness", -0.1}}, "negative aggressiveness");
    rejects({{"maxTotalQuantity", -1}}, "negative quantity budget");

    auto ok = DirectionalParams::fromJson({{"side", "SELL"}, {"orderSize", 300}});
    check(!ok.isBuy(), "SELL parses");
    check(ok.aggressiveness == 1.0, "defaults to fully aggressive");
    std::cout << "  parameter validation checked" << std::endl;
}

void test_aggressiveness_maps_across_the_spread() {
    // A 20-tick spread so the interpolation is unambiguous.
    auto priceAt = [](double aggr, bool buy) {
        Fixture f;
        f.quote(499.90, 500.10);
        f.params = {{"side", buy ? "BUY" : "SELL"}, {"aggressiveness", aggr},
                    {"arrivalIntervalMs", 100}, {"orderSize", 100}};
        DirectionalTrader d;
        auto c = f.ctx(); d.onStart(c);
        f.drive(d, 2);
        return f.gateway.submissions.empty() ? 0.0 : f.gateway.submissions[0].price;
    };

    check(std::fabs(priceAt(0.0, true) - 499.90) < 1e-9,
          "aggressiveness 0 rests at the near touch (bid)");
    check(std::fabs(priceAt(0.5, true) - 500.00) < 1e-9,
          "aggressiveness 0.5 sits mid-spread");
    check(std::fabs(priceAt(1.0, true) - 500.10) < 1e-9,
          "aggressiveness 1 reaches the far touch (ask)");

    check(std::fabs(priceAt(0.0, false) - 500.10) < 1e-9,
          "seller at 0 rests at the near touch (ask)");
    check(std::fabs(priceAt(1.0, false) - 499.90) < 1e-9,
          "seller at 1 reaches the far touch (bid)");
    std::cout << "  aggressiveness mapping checked" << std::endl;
}

void test_extra_cross_ticks_reach_beyond_the_touch() {
    Fixture f;
    f.quote(499.90, 500.10);
    f.params = {{"side", "BUY"}, {"aggressiveness", 1.0}, {"extraCrossTicks", 5},
                {"arrivalIntervalMs", 100}, {"orderSize", 100}};
    DirectionalTrader d;
    auto c = f.ctx(); d.onStart(c);
    f.drive(d, 2);
    check(std::fabs(f.gateway.submissions[0].price - 500.15) < 1e-9,
          "extraCrossTicks reaches past the far touch (got " +
              std::to_string(f.gateway.submissions[0].price) + ")");
    std::cout << "  extra cross ticks checked" << std::endl;
}

void test_respects_arrival_rate() {
    Fixture f;
    f.quote();
    f.params = {{"side", "BUY"}, {"arrivalIntervalMs", 500}, {"orderSize", 100}};
    DirectionalTrader d;
    auto c = f.ctx(); d.onStart(c);
    f.drive(d, 50, 100);   // 5 seconds at 100ms ticks => ~10 orders at 500ms
    const int sent = static_cast<int>(f.gateway.submissions.size());
    check(sent >= 8 && sent <= 12,
          "arrival interval governs order count, not tick cadence (got " +
              std::to_string(sent) + ")");
    std::cout << "  arrival rate checked" << std::endl;
}

void test_stops_at_duration() {
    Fixture f;
    f.quote();
    f.params = {{"side", "BUY"}, {"arrivalIntervalMs", 100}, {"orderSize", 100},
                {"durationSeconds", 2.0}};
    DirectionalTrader d;
    auto c = f.ctx(); d.onStart(c);
    f.drive(d, 60, 100);
    check(d.isComplete(), "completes once its duration elapses");
    const int sent = static_cast<int>(f.gateway.submissions.size());
    check(sent <= 22, "sends no orders past its duration (got " + std::to_string(sent) + ")");
    std::cout << "  duration checked" << std::endl;
}

void test_respects_quantity_budget() {
    Fixture f;
    f.quote();
    f.params = {{"side", "BUY"}, {"arrivalIntervalMs", 100}, {"orderSize", 300},
                {"maxTotalQuantity", 1000}};
    DirectionalTrader d;
    auto c = f.ctx(); d.onStart(c);
    f.drive(d, 60, 100);

    long long sent = 0;
    for (const auto& s : f.gateway.submissions) sent += s.size;
    check(sent == 1000, "sends exactly the budget, not a rounded-up multiple of orderSize");
    check(f.gateway.submissions.back().size == 100, "final order is the remainder");
    check(d.isComplete(), "completes once the budget is exhausted");
    std::cout << "  quantity budget checked" << std::endl;
}

void test_price_limit_stops_chasing() {
    Fixture f;
    f.quote(499.90, 500.10);
    f.params = {{"side", "BUY"}, {"arrivalIntervalMs", 100}, {"orderSize", 100},
                {"aggressiveness", 1.0}, {"priceLimit", 500.10}};
    DirectionalTrader d;
    auto c = f.ctx(); d.onStart(c);
    f.drive(d, 5);
    const size_t before = f.gateway.submissions.size();
    check(before > 0, "trades while inside the limit");

    // The market runs away past the limit: a persistent taker without a limit would
    // chase it indefinitely.
    f.quote(500.50, 500.70);
    f.drive(d, 20);
    check(f.gateway.submissions.size() == before, "sends nothing once price exceeds the limit");
    check(d.metrics()["skipped_price_limit"].get<long long>() > 0, "skips are reported");
    std::cout << "  price limit checked" << std::endl;
}

void test_marketable_residual_is_cancelled() {
    Fixture f;
    f.quote();
    f.params = {{"side", "BUY"}, {"arrivalIntervalMs", 1000}, {"orderSize", 100},
                {"aggressiveness", 1.0}, {"residualCancelMs", 300}};
    DirectionalTrader d;
    auto c = f.ctx(); d.onStart(c);
    // The first order arrives one full interval in (1000ms at 100ms ticks), and the
    // second would arrive at 2000ms; this window holds exactly one.
    f.drive(d, 11, 100);
    check(f.gateway.submissions.size() == 1, "one marketable order sent");
    check(f.gateway.canceled.empty(), "not cancelled immediately");

    // Nothing filled it, so the remainder would rest as a BID -- an actor meant to
    // consume ask liquidity would start supplying bid liquidity instead.
    f.drive(d, 5, 100);
    check(!f.gateway.canceled.empty(), "unfilled marketable remainder is cancelled");
    check(d.metrics()["residuals_cancelled"].get<long long>() > 0, "cancellation reported");
    std::cout << "  marketable residual cancellation checked" << std::endl;
}

void test_passive_orders_are_left_to_rest() {
    Fixture f;
    f.quote();
    f.params = {{"side", "BUY"}, {"arrivalIntervalMs", 200}, {"orderSize", 100},
                {"aggressiveness", 0.0}, {"residualCancelMs", 200}};
    DirectionalTrader d;
    auto c = f.ctx(); d.onStart(c);
    f.drive(d, 30, 100);
    // Resting IS the point of a passive order; cancelling it would make
    // aggressiveness 0 meaningless.
    check(f.gateway.canceled.empty(), "passive orders are never expired as residuals");
    check(f.tracker.activeCount() > 1, "passive orders accumulate in the book");
    std::cout << "  passive orders left resting checked" << std::endl;
}

void test_only_trades_its_configured_side() {
    for (const char* side : {"BUY", "SELL"}) {
        Fixture f;
        f.quote();
        f.params = {{"side", side}, {"arrivalIntervalMs", 100}, {"orderSize", 100}};
        DirectionalTrader d;
        auto c = f.ctx(); d.onStart(c);
        f.drive(d, 30, 100);
        const bool wantBuy = std::string(side) == "BUY";
        for (const auto& s : f.gateway.submissions)
            check(s.buy == wantBuy, std::string("every order is on the ") + side + " side");
    }
    std::cout << "  one-sidedness checked" << std::endl;
}

void test_waits_for_a_market() {
    Fixture f;   // no quotes
    f.params = {{"side", "BUY"}, {"arrivalIntervalMs", 100}, {"orderSize", 100}};
    DirectionalTrader d;
    auto c = f.ctx(); d.onStart(c);
    f.drive(d, 10);
    check(f.gateway.submissions.empty(), "does not trade without a two-sided market");
    check(d.metrics()["skipped_no_market"].get<long long>() > 0, "skips are reported");

    f.quote();
    f.drive(d, 3);
    check(!f.gateway.submissions.empty(), "trades once a market exists");
    std::cout << "  market guard checked" << std::endl;
}

}  // namespace

void run_directional_trader_tests() {
    std::cout << "--- DirectionalTrader Tests ---" << std::endl;
    test_param_validation();
    test_aggressiveness_maps_across_the_spread();
    test_extra_cross_ticks_reach_beyond_the_touch();
    test_respects_arrival_rate();
    test_stops_at_duration();
    test_respects_quantity_budget();
    test_price_limit_stops_chasing();
    test_marketable_residual_is_cancelled();
    test_passive_orders_are_left_to_rest();
    test_only_trades_its_configured_side();
    test_waits_for_a_market();
    if (failures != 0) {
        std::cout << failures << " DirectionalTrader assertion(s) FAILED" << std::endl;
        std::exit(1);
    }
    std::cout << "All DirectionalTrader tests passed." << std::endl;
}
