#include "fake_gateway.h"
#include "strategies/market_maker.h"

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
    RandomSource     rng{11};
    std::ostringstream sink;
    EventLog         log{"test_run", "mm", sink};
    FakeGateway      gateway{tracker};
    nlohmann::json   params;
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

    StrategyContext ctx() {
        return StrategyContext{market, tracker, position, rng, gateway, log,
                               params, now, kTick, 0.0};
    }
    void advance(int ms) { now += std::chrono::milliseconds(ms); }
    void quote(double bid = 499.90, double ask = 500.10) {
        market.bestBid = bid; market.bestAsk = ask;
        market.bestBidSize = 500; market.bestAskSize = 500;
    }
    void drive(MarketMaker& mm, int ticks, int stepMs = 100) {
        for (int i = 0; i < ticks; ++i) { advance(stepMs); auto c = ctx(); mm.onTick(c); }
    }
    // Latest live quote on a side, or 0 if none.
    double liveQuote(bool buy) {
        double price = 0.0;
        for (const auto& s : gateway.submissions)
            if (s.buy == buy && tracker.isLiveOrder(s.id)) price = s.price;
        return price;
    }
};

void test_param_validation() {
    auto rejects = [](nlohmann::json j, const std::string& why) {
        try {
            MarketMakerParams::fromJson(j);
            std::cout << "  [FAIL] should have rejected: " << why << std::endl;
            ++failures;
        } catch (const std::runtime_error&) {}
    };
    rejects({{"quoteSize", 0}}, "zero quote size");
    rejects({{"halfSpreadTicks", 0}}, "zero half-spread would self-cross");
    rejects({{"requoteThresholdTicks", 0}}, "zero threshold means constant churn");
    rejects({{"inventoryLimit", 0}}, "zero inventory limit");
    rejects({{"inventorySkewTicksPerUnit", -1.0}}, "negative skew amplifies inventory");
    rejects({{"referenceMode", "vwap"}}, "unsupported reference mode");

    auto ok = MarketMakerParams::fromJson({{"quoteSize", 300}});
    check(ok.quoteSize == 300, "valid config parses");
    check(ok.referenceMode == "mid", "defaults applied");
    std::cout << "  parameter validation checked" << std::endl;
}

void test_quotes_both_sides_around_reference() {
    Fixture f;
    f.quote(499.90, 500.10);          // mid 500.00, wide enough to quote inside
    f.params = {{"quoteSize", 200}, {"halfSpreadTicks", 2},
                {"inventorySkewTicksPerUnit", 0.0}};
    MarketMaker mm;
    auto c = f.ctx(); mm.onStart(c);
    f.drive(mm, 2);

    check(std::fabs(f.liveQuote(true) - 499.98) < 1e-9,
          "bid is 2 ticks below the mid (got " + std::to_string(f.liveQuote(true)) + ")");
    check(std::fabs(f.liveQuote(false) - 500.02) < 1e-9,
          "ask is 2 ticks above the mid (got " + std::to_string(f.liveQuote(false)) + ")");
    check(f.gateway.submissions.size() == 2, "exactly one quote per side");
    for (const auto& s : f.gateway.submissions)
        check(s.size == 200, "quotes are quoteSize");
    std::cout << "  two-sided quoting checked" << std::endl;
}

void test_inventory_skew_sign_and_cap() {
    MarketMakerParams p;
    p.quoteSize = 200;
    p.inventorySkewTicksPerUnit = 1.0;
    p.maxInventorySkewTicks = 5;

    check(MarketMaker::inventorySkewTicks(p, 0) == 0, "flat inventory => no skew");
    // Long inventory must shift quotes DOWN (positive skew) to shed the position.
    check(MarketMaker::inventorySkewTicks(p, 200) == 1, "long one lot => down 1 tick");
    check(MarketMaker::inventorySkewTicks(p, 600) == 3, "long three lots => down 3 ticks");
    check(MarketMaker::inventorySkewTicks(p, -600) == -3, "short three lots => up 3 ticks");
    check(MarketMaker::inventorySkewTicks(p, 100000) == 5, "skew is capped");
    check(MarketMaker::inventorySkewTicks(p, -100000) == -5, "skew is capped on the short side");

    p.inventorySkewTicksPerUnit = 0.0;
    check(MarketMaker::inventorySkewTicks(p, 5000) == 0, "skew disabled by default value");
    std::cout << "  inventory skew checked" << std::endl;
}

void test_quotes_shift_down_when_long() {
    Fixture f;
    f.quote(499.50, 500.50);          // deliberately wide so nothing clamps
    f.params = {{"quoteSize", 200}, {"halfSpreadTicks", 2},
                {"inventorySkewTicksPerUnit", 1.0}, {"inventoryLimit", 10000}};
    MarketMaker mm;
    auto c = f.ctx(); mm.onStart(c);
    f.drive(mm, 2);
    const double bid0 = f.liveQuote(true), ask0 = f.liveQuote(false);

    f.position.onFill(true, 600, 500.00);   // long three lots => 3 ticks down
    f.drive(mm, 3);

    check(std::fabs(f.liveQuote(true) - (bid0 - 0.03)) < 1e-9,
          "bid moves down 3 ticks when long (got " + std::to_string(f.liveQuote(true)) + ")");
    check(std::fabs(f.liveQuote(false) - (ask0 - 0.03)) < 1e-9,
          "ask moves down 3 ticks when long, making it easier to sell");
    std::cout << "  long-inventory skew checked" << std::endl;
}

void test_requote_threshold_prevents_churn() {
    Fixture f;
    f.quote(499.50, 500.50);
    f.params = {{"quoteSize", 200}, {"halfSpreadTicks", 2},
                {"requoteThresholdTicks", 3}, {"inventorySkewTicksPerUnit", 0.0}};
    MarketMaker mm;
    auto c = f.ctx(); mm.onStart(c);
    f.drive(mm, 2);
    const size_t afterSeed = f.gateway.submissions.size();

    // A 1-tick reference move is inside the threshold: surrendering queue position
    // for it would be pure churn.
    f.quote(499.51, 500.51);
    f.drive(mm, 5);
    check(f.gateway.submissions.size() == afterSeed, "sub-threshold drift does not requote");
    check(f.gateway.canceled.empty(), "sub-threshold drift cancels nothing");

    // A 4-tick move exceeds it and must requote.
    f.quote(499.58, 500.58);
    f.drive(mm, 3);
    check(f.gateway.submissions.size() > afterSeed, "a move past the threshold requotes");
    std::cout << "  requote threshold checked" << std::endl;
}

void test_withdraws_side_at_inventory_limit() {
    Fixture f;
    f.quote(499.50, 500.50);
    f.params = {{"quoteSize", 200}, {"halfSpreadTicks", 2},
                {"inventoryLimit", 500}, {"inventorySkewTicksPerUnit", 0.0}};
    MarketMaker mm;
    auto c = f.ctx(); mm.onStart(c);
    f.drive(mm, 2);
    check(f.liveQuote(true) > 0.0 && f.liveQuote(false) > 0.0, "starts quoting both sides");

    // Long past the limit: the bid must be withdrawn, the ask must stay.
    f.position.onFill(true, 600, 500.00);
    f.drive(mm, 3);
    check(f.liveQuote(true) == 0.0, "bid is withdrawn at the long inventory limit");
    check(f.liveQuote(false) > 0.0, "ask keeps quoting so the position can be reduced");

    // Back inside the limit: the bid returns.
    f.position.onFill(false, 600, 500.00);
    f.drive(mm, 3);
    check(f.liveQuote(true) > 0.0, "bid returns once inventory is back inside the limit");
    std::cout << "  inventory limit checked" << std::endl;
}

void test_withdraws_ask_when_short() {
    Fixture f;
    f.quote(499.50, 500.50);
    f.params = {{"quoteSize", 200}, {"halfSpreadTicks", 2},
                {"inventoryLimit", 500}, {"inventorySkewTicksPerUnit", 0.0}};
    MarketMaker mm;
    auto c = f.ctx(); mm.onStart(c);
    f.drive(mm, 2);

    f.position.onFill(false, 600, 500.00);   // short past the limit
    f.drive(mm, 3);
    check(f.liveQuote(false) == 0.0, "ask is withdrawn at the short inventory limit");
    check(f.liveQuote(true) > 0.0, "bid keeps quoting so the short can be covered");
    std::cout << "  short-side inventory limit checked" << std::endl;
}

void test_never_crosses_the_market() {
    Fixture f;
    // A 2-tick market with a maker wanting a 10-tick half-spread inside it: naive
    // arithmetic would place the bid above the ask and trade with the book.
    f.quote(499.99, 500.01);
    f.params = {{"quoteSize", 200}, {"halfSpreadTicks", 10},
                {"inventorySkewTicksPerUnit", 0.0}};
    MarketMaker mm;
    auto c = f.ctx(); mm.onStart(c);
    f.drive(mm, 3);

    for (const auto& s : f.gateway.submissions) {
        if (s.buy) check(s.price < 500.01 - 1e-9, "bid never reaches the best ask");
        else       check(s.price > 499.99 + 1e-9, "ask never reaches the best bid");
    }
    std::cout << "  passivity checked" << std::endl;
}

void test_large_skew_cannot_cross_either() {
    Fixture f;
    f.quote(499.99, 500.01);
    f.params = {{"quoteSize", 200}, {"halfSpreadTicks", 1},
                {"inventorySkewTicksPerUnit", 5.0}, {"maxInventorySkewTicks", 50},
                {"inventoryLimit", 100000}};
    MarketMaker mm;
    auto c = f.ctx(); mm.onStart(c);
    f.position.onFill(false, 4000, 500.00);   // very short => big upward skew
    f.drive(mm, 4);

    for (const auto& s : f.gateway.submissions) {
        if (s.buy) check(s.price < 500.01 - 1e-9, "skewed bid still does not cross");
        else       check(s.price > 499.99 + 1e-9, "skewed ask still does not cross");
    }
    std::cout << "  skew passivity checked" << std::endl;
}

void test_waits_for_a_reference() {
    Fixture f;   // no market, no last price
    f.params = {{"quoteSize", 200}};
    MarketMaker mm;
    auto c = f.ctx(); mm.onStart(c);
    f.drive(mm, 5);
    check(f.gateway.submissions.empty(), "does not quote without a reference price");

    f.quote();
    f.drive(mm, 2);
    check(!f.gateway.submissions.empty(), "quotes once a reference exists");
    std::cout << "  reference guard checked" << std::endl;
}

void test_last_price_reference_mode() {
    Fixture f;
    f.params = {{"quoteSize", 200}, {"halfSpreadTicks", 2},
                {"referenceMode", "last"}, {"inventorySkewTicksPerUnit", 0.0}};
    f.market.lastPrice = 501.00;       // no BBO at all, only a last trade
    MarketMaker mm;
    auto c = f.ctx(); mm.onStart(c);
    f.drive(mm, 2);
    check(std::fabs(f.liveQuote(true) - 500.98) < 1e-9, "bid is quoted around the last price");
    check(std::fabs(f.liveQuote(false) - 501.02) < 1e-9, "ask is quoted around the last price");
    std::cout << "  last-price reference checked" << std::endl;
}

void test_metrics_are_reported() {
    Fixture f;
    f.quote(499.50, 500.50);
    f.params = {{"quoteSize", 200}, {"halfSpreadTicks", 2},
                {"inventorySkewTicksPerUnit", 1.0}, {"inventoryLimit", 10000}};
    MarketMaker mm;
    auto c = f.ctx(); mm.onStart(c);
    f.drive(mm, 5);

    const auto sub = f.gateway.submissions.front();
    f.gateway.fill(sub.id, 200);
    Fill fill{sub.id, sub.price, 200, sub.buy, true, 0};
    auto c2 = f.ctx(); mm.onFill(c2, fill);
    f.position.onFill(sub.buy, 200, sub.price);
    f.drive(mm, 3);

    auto m = mm.metrics();
    check(m["quote_updates"].get<long long>() >= 2, "quote updates counted");
    check(m["bid_shares_filled"].get<long long>() +
          m["ask_shares_filled"].get<long long>() == 200, "filled shares counted");
    check(m["mean_quoted_spread_ticks"].get<double>() > 0.0, "quoted spread recorded");
    check(m["bid_quoted_fraction"].get<double>() > 0.0, "quoted fraction recorded");
    std::cout << "  metrics checked" << std::endl;
}

}  // namespace

void run_market_maker_tests() {
    std::cout << "--- MarketMaker Tests ---" << std::endl;
    test_param_validation();
    test_quotes_both_sides_around_reference();
    test_inventory_skew_sign_and_cap();
    test_quotes_shift_down_when_long();
    test_requote_threshold_prevents_churn();
    test_withdraws_side_at_inventory_limit();
    test_withdraws_ask_when_short();
    test_never_crosses_the_market();
    test_large_skew_cannot_cross_either();
    test_waits_for_a_reference();
    test_last_price_reference_mode();
    test_metrics_are_reported();
    if (failures != 0) {
        std::cout << failures << " MarketMaker assertion(s) FAILED" << std::endl;
        std::exit(1);
    }
    std::cout << "All MarketMaker tests passed." << std::endl;
}
