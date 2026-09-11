#pragma once

#include "strategy.h"

#include <chrono>
#include <string>
#include <vector>

// Controlled liquidity sweep: consume a target quantity from the book in one
// deliberate burst, then get out of the way.
//
// This is the cleanest observable market event in the suite. Everything else in a
// scenario runs continuously; a sweep happens at a known instant with a known size,
// which is what makes "quantity -> levels consumed -> slippage -> impact -> recovery"
// measurable at all.
//
// Two engine facts shape the design:
//
//   1. There is no market order type. A sweep is a marketable LIMIT, so whatever
//      does not execute RESTS in the book -- as a bid above the old best bid, which
//      would distort every measurement taken after it. The residual is therefore
//      cancelled explicitly once the sweep settles, not left to the runtime.
//
//   2. Matching is asynchronous. Submitting does not mean filling, so quantity sent
//      is tracked separately from quantity filled, and the invariant that matters
//      (never execute more than targetQuantity) is enforced on the SENT side, which
//      bounds fills regardless of match timing.
//
// This strategy is deliberately the `immediate` case of a general parent-order
// executor. Phase 11 generalises it into parent_executor with sliced /
// participation / passive-first modes rather than writing a second executor.
struct SweepParams {
    std::string side = "BUY";        // BUY consumes asks, SELL consumes bids
    int    targetQuantity = 0;       // required, > 0

    // How far to reach into the book. Exactly one of these is required.
    double limitPrice = 0.0;         // explicit worst acceptable price
    int    maxLevels  = 0;           // or: derive the limit from the book at arrival

    // Slicing. childOrders=false sends the whole quantity as one order.
    bool   childOrders   = false;
    int    childQuantity = 0;        // required when childOrders is true
    int    childDelayMs  = 50;

    int    startDelayMs  = 0;        // wait this long after start before arming
    int    settleMs      = 400;      // wait for fills after the last child, then cancel
    int    maxDurationMs = 15000;    // hard stop, so a stuck sweep cannot hang a run

    static SweepParams fromJson(const nlohmann::json& j);
    bool isBuy() const { return side == "BUY"; }
};

class Sweep : public Strategy {
public:
    void onStart(StrategyContext& ctx) override;
    void onTick(StrategyContext& ctx) override;
    void onFill(StrategyContext& ctx, const Fill& fill) override;
    void onStop(StrategyContext& ctx) override;
    nlohmann::json metrics() const override;
    bool isComplete() const override { return complete_; }

private:
    bool arm(StrategyContext& ctx);      // capture arrival state; false if not ready
    void cancelResidual(StrategyContext& ctx);

    SweepParams params_;
    bool armed_    = false;
    bool complete_ = false;
    bool residualCancelled_ = false;

    std::chrono::steady_clock::time_point startedAt_{};
    std::chrono::steady_clock::time_point armedAt_{};
    std::chrono::steady_clock::time_point lastChildAt_{};
    std::chrono::steady_clock::time_point allSentAt_{};
    std::chrono::steady_clock::time_point finishedAt_{};
    bool allSent_ = false;

    // Arrival snapshot, captured once at arm time.
    double arrivalBestBid_ = 0.0;
    double arrivalBestAsk_ = 0.0;
    double arrivalMid_     = 0.0;
    double limitPrice_     = 0.0;
    long long arrivalDepthWithinLimit_ = 0;
    int    arrivalLevelsWithinLimit_   = 0;

    long long sentQuantity_   = 0;
    long long filledQuantity_ = 0;
    double    notional_       = 0.0;   // sum(price * size), for VWAP
    int       childCount_     = 0;
    int       rejectedChildren_ = 0;
    std::vector<unsigned int> childIds_;
};
