#pragma once

#include "strategy.h"

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Baseline liquidity provider: maintains a configurable multi-level two-sided book
// around a reference price.  This is the foundation of every experiment -- without a
// standing book there is nothing for other participants to trade against.
//
// It works by reconciliation, not by scripting: each tick it compares the DESIRED
// ladder against the orders it actually has live, submits what is missing, and
// cancels anything that no longer belongs.  That single mechanism covers seeding,
// replenishment after fills, and re-centering, and it is naturally idempotent -- a
// missed tick or a racing fill just gets fixed on the next pass.
//
// Prices are keyed by integer tick index rather than by double.  The engine keys its
// book on exact double equality, so an unrounded price silently creates a new level
// per floating-point wobble.
struct LadderParams {
    double referencePrice           = 0.0;   // required
    int    bidLevels                = 10;
    int    askLevels                = 10;
    int    halfSpreadTicks          = 1;     // best bid/ask sit this far from reference
    int    levelSpacingTicks        = 1;     // gap between consecutive levels
    int    averageQuantity          = 500;
    int    quantityVariance         = 0;     // 0 => a rigid, perfectly uniform ladder
    int    levelSpacingJitterTicks  = 0;     // 0 => perfectly even spacing
    double maxDistanceFromReference = 0.0;   // 0 => unlimited
    int    replenishDelayMs         = 0;     // pause before refilling a consumed level
    bool   recenterOnMid            = false;
    int    recenterThresholdTicks   = 0;

    // Inventory-driven reference shift.
    //
    // Without this the provider is an infinitely elastic wall: it refills every
    // consumed level at the same price forever, so the mid it quotes is a fixed
    // point that no amount of order flow can move, and price discovery is impossible.
    // Measured directly in Experiment 4 -- price moved 12 ticks with noise alone and
    // only 2 ticks once a rigid provider was added, while the provider silently
    // absorbed +1007 shares of inventory without ever responding to it.
    //
    // The response is the classic inventory control of Ho-Stoll / Avellaneda-Stoikov:
    // as the provider accumulates a long position it shifts its whole ladder DOWN,
    // making its bids less attractive and its asks more attractive, until flow
    // reverses. Directional pressure now moves price instead of being absorbed.
    //
    // skew_ticks = clamp((inventory - target) / unitShares * ticksPerUnit, +/- maxSkew)
    // effective_reference = referencePrice shifted DOWN by skew_ticks
    int    inventoryTarget          = 0;
    int    inventoryUnitShares      = 0;     // 0 => defaults to averageQuantity
    double inventorySkewTicksPerUnit = 0.0;  // 0 => disabled (rigid wall)
    int    maxInventorySkewTicks    = 50;

    std::string mode                = "normal";

    // Throws std::runtime_error with an actionable message on invalid input.
    static LadderParams fromJson(const nlohmann::json& j);
};

class LiquidityProvider : public Strategy {
public:
    struct Level {
        long long tickIndex = 0;
        double    price     = 0.0;
        int       size      = 0;
        bool      buy       = false;
    };

    // Pure function of (params, reference, rng): the ladder the provider WANTS.
    // Separated out so ladder shape can be unit-tested with no engine, no threads,
    // and no strategy state.
    static std::vector<Level> buildLadder(const LadderParams& params,
                                          double referencePrice,
                                          double tickSize,
                                          RandomSource& rng);

    // Ticks to shift the ladder for a given inventory. Positive means "shift down".
    // Pure and static so the inventory response can be unit-tested on its own.
    static long long inventorySkewTicks(const LadderParams& params, int inventory);

    void onStart(StrategyContext& ctx) override;
    void onTick(StrategyContext& ctx) override;
    void onFill(StrategyContext& ctx, const Fill& fill) override;
    nlohmann::json metrics() const override;

private:
    struct Slot {
        Level level;
        unsigned int orderId = 0;   // 0 => nothing live here
        std::chrono::steady_clock::time_point readyAt{};  // replenish no earlier than
    };

    void rebuildDesired(StrategyContext& ctx, double centerPrice);
    void drainFills(StrategyContext& ctx);

    LadderParams params_;
    double reference_ = 0.0;        // anchor; moved by recenterOnMid
    long long ladderCenterTick_ = 0; // tick index the live ladder is built around
    bool   started_   = false;

    // Desired ladder, keyed by tick index. Insertion/removal only on rebuild.
    std::map<long long, Slot> slots_;

    // onFill runs on the fill-stream thread; onTick on the decision thread.  Rather
    // than sharing slots_ across both, fills are queued here and drained by the tick.
    // The timestamp is captured at fill time so the replenish delay is measured from
    // the execution, not from whenever the next tick happens to run.
    std::mutex fillMutex_;
    std::vector<std::pair<unsigned int, std::chrono::steady_clock::time_point>> pendingFills_;

    // metrics
    long long replenishments_   = 0;
    long long recenters_        = 0;
    long long skewShifts_       = 0;
    long long maxAbsSkewTicks_  = 0;
    long long maxAbsInventory_  = 0;
    long long minCenterTick_    = 0;
    long long maxCenterTick_    = 0;
    long long fillsObserved_    = 0;
    long long sharesFilled_     = 0;
    long long ticksObserved_    = 0;
    long long liveLevelSamples_ = 0;
    long long bidDepthSamples_  = 0;
    long long askDepthSamples_  = 0;
    double    secondsIncomplete_ = 0.0;
    std::chrono::steady_clock::time_point lastTickAt_{};
};
