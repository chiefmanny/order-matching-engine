#include "fake_gateway.h"
#include "strategies/layering_demo.h"

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
    RandomSource     rng{23};
    std::ostringstream sink;
    EventLog         log{"test_run", "layer", sink};
    FakeGateway      gateway{tracker};
    nlohmann::json   params;
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

    StrategyContext ctx() {
        return StrategyContext{market, tracker, position, rng, gateway, log,
                               params, now, kTick, 0.0};
    }
    void advance(int ms) { now += std::chrono::milliseconds(ms); }
    void quote(double bid = 499.99, double ask = 500.01) {
        market.bestBid = bid; market.bestAsk = ask;
        market.bestBidSize = 500; market.bestAskSize = 500;
    }
    void drive(LayeringDemo& d, int ticks, int stepMs = 100) {
        for (int i = 0; i < ticks && !d.isComplete(); ++i) {
            advance(stepMs); auto c = ctx(); d.onTick(c);
        }
    }
};

void test_param_validation() {
    auto rejects = [](nlohmann::json j, const std::string& why) {
        try {
            LayeringParams::fromJson(j);
            std::cout << "  [FAIL] should have rejected: " << why << std::endl;
            ++failures;
        } catch (const std::runtime_error&) {}
    };
    rejects({{"side", "MIDDLE"}}, "bad side");
    rejects({{"layers", 0}}, "zero layers");
    rejects({{"layerSize", 0}}, "zero layer size");
    // A layer at the touch could actually trade, which the pattern depends on not
    // happening; refusing it keeps the demonstration honest about what it shows.
    rejects({{"startOffsetTicks", 0}}, "offset of zero would sit at the touch");
    rejects({{"layerSpacingTicks", 0}}, "zero spacing");
    std::cout << "  parameter validation checked" << std::endl;
}

void test_layers_are_placed_behind_the_touch() {
    Fixture f; f.quote(499.99, 500.01);
    f.params = {{"side", "BUY"}, {"layers", 5}, {"layerSize", 2000},
                {"startOffsetTicks", 2}, {"layerSpacingTicks", 1},
                {"holdMs", 500}, {"cycleIntervalMs", 1000}};
    LayeringDemo d;
    auto c = f.ctx(); d.onStart(c);
    f.drive(d, 1);

    check(f.gateway.submissions.size() == 5, "all five layers placed at once");
    for (const auto& s : f.gateway.submissions) {
        check(s.buy, "layers are on the configured side");
        check(s.size == 2000, "layers carry the configured size");
        // Never at or through the touch: displayed, not executable.
        check(s.price <= 499.97 + 1e-9,
              "layer rests behind the touch (got " + std::to_string(s.price) + ")");
    }
    // 2,3,4,5,6 ticks below 499.99.
    check(std::fabs(f.gateway.submissions[0].price - 499.97) < 1e-9, "first layer offset");
    check(std::fabs(f.gateway.submissions[4].price - 499.93) < 1e-9, "last layer offset");
    std::cout << "  layer placement checked" << std::endl;
}

void test_layers_are_withdrawn_after_the_hold() {
    Fixture f; f.quote();
    f.params = {{"layers", 4}, {"layerSize", 1000}, {"holdMs", 500},
                {"cycleIntervalMs", 1000}};
    LayeringDemo d;
    auto c = f.ctx(); d.onStart(c);
    f.drive(d, 1);
    check(f.tracker.activeCount() == 4, "layers resting during the hold");
    check(f.gateway.canceled.empty(), "not withdrawn before the hold elapses");

    f.drive(d, 7, 100);
    check(f.gateway.canceled.size() >= 4, "all layers withdrawn after the hold");
    check(f.tracker.activeCount() == 0, "nothing left resting");
    std::cout << "  withdrawal checked" << std::endl;
}

void test_cycles_repeat_and_stop_at_the_limit() {
    Fixture f; f.quote();
    f.params = {{"layers", 3}, {"layerSize", 1000}, {"holdMs", 300},
                {"cycleIntervalMs", 300}, {"cycles", 3}};
    LayeringDemo d;
    auto c = f.ctx(); d.onStart(c);
    f.drive(d, 60, 100);

    auto m = d.metrics();
    check(m["cycles_completed"].get<long long>() == 3, "stops after the configured cycles");
    check(m["layers_placed"].get<long long>() == 9, "three cycles of three layers");
    check(d.isComplete(), "reports complete");
    check(f.tracker.activeCount() == 0, "leaves nothing behind");
    std::cout << "  cycling checked" << std::endl;
}

void test_cancel_ratio_is_the_headline_signal() {
    Fixture f; f.quote();
    f.params = {{"layers", 5}, {"layerSize", 2000}, {"holdMs", 300},
                {"cycleIntervalMs", 300}, {"cycles", 4}};
    LayeringDemo d;
    auto c = f.ctx(); d.onStart(c);
    f.drive(d, 80, 100);

    auto m = d.metrics();
    // Essentially everything posted is withdrawn without trading. This is the number
    // surveillance keys on, and the reason the actor is trivially separable.
    check(std::fabs(m["layer_cancel_ratio"].get<double>() - 1.0) < 1e-9,
          "every displayed layer is withdrawn (got " +
              std::to_string(m["layer_cancel_ratio"].get<double>()) + ")");
    check(m["layers_filled_shares"].get<long long>() == 0, "no layer traded");
    check(m["displayed_quantity_total"].get<long long>() == 40000,
          "total displayed quantity is reported");
    std::cout << "  cancel ratio signal checked" << std::endl;
}

void test_opposing_order_is_on_the_other_side() {
    Fixture f; f.quote();
    f.params = {{"side", "BUY"}, {"layers", 3}, {"layerSize", 2000},
                {"holdMs", 400}, {"cycleIntervalMs", 400},
                {"tradeOpposite", true}, {"opposingSize", 200}};
    LayeringDemo d;
    auto c = f.ctx(); d.onStart(c);
    f.drive(d, 1);

    int buys = 0, sells = 0;
    for (const auto& s : f.gateway.submissions) (s.buy ? buys : sells)++;
    check(buys == 3, "three displayed layers on the buy side");
    // The genuine order goes the OTHER way: the layers are cover, not the trade.
    check(sells == 1, "one small order on the opposite side");
    for (const auto& s : f.gateway.submissions)
        if (!s.buy) check(s.size == 200, "the opposing order is small");
    std::cout << "  opposing order checked" << std::endl;
}

void test_waits_for_a_market() {
    Fixture f;   // no quotes
    f.params = {{"layers", 3}, {"layerSize", 1000}};
    LayeringDemo d;
    auto c = f.ctx(); d.onStart(c);
    f.drive(d, 10);
    check(f.gateway.submissions.empty(), "does nothing without a two-sided market");
    f.quote();
    f.drive(d, 2);
    check(!f.gateway.submissions.empty(), "acts once a market exists");
    std::cout << "  market guard checked" << std::endl;
}

}  // namespace

void run_layering_demo_tests() {
    std::cout << "--- LayeringDemo Tests ---" << std::endl;
    test_param_validation();
    test_layers_are_placed_behind_the_touch();
    test_layers_are_withdrawn_after_the_hold();
    test_cycles_repeat_and_stop_at_the_limit();
    test_cancel_ratio_is_the_headline_signal();
    test_opposing_order_is_on_the_other_side();
    test_waits_for_a_market();
    if (failures != 0) {
        std::cout << failures << " LayeringDemo assertion(s) FAILED" << std::endl;
        std::exit(1);
    }
    std::cout << "All LayeringDemo tests passed." << std::endl;
}
