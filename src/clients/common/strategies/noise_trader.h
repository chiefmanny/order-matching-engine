#pragma once

#include "strategy.h"

#include <chrono>
#include <string>

// Background order flow: the participant that turns a standing book into a market.
//
// Without it the liquidity provider's ladder just sits there -- no trades, no price
// discovery, no candles. With it, orders arrive, some cross the spread and execute,
// some rest and are later cancelled, and a price series emerges from the interaction
// rather than from any single actor's script.
//
// It is deliberately NOT smart. It has no view, no inventory target and no signal;
// it is the undifferentiated flow that everything else is measured against. If a
// scenario's interesting behavior comes from the noise trader, the scenario is wrong.
//
// Two knobs decide its character:
//   * aggressiveProbability - how often it crosses the spread (creates trades)
//   * cancelProbability     - how often it pulls a resting order (creates churn)
struct NoiseParams {
    int    orderIntervalMs        = 300;  // mean gap between actions
    int    intervalJitterMs       = 150;  // uniform +/- jitter around that gap
    int    meanQuantity           = 100;
    int    quantityVariance       = 50;
    double aggressiveProbability  = 0.20;
    double cancelProbability      = 0.15;
    double buyProbability         = 0.50;
    int    passivePlacementTicks  = 5;    // rest 0..N ticks behind the near touch
    int    aggressiveCrossTicks   = 0;    // 0 => take exactly at the far touch
    int    maxOpenOrders          = 20;
    // Used only while the book is not two-sided, so noise can bootstrap a market on
    // its own (that is what makes the `noise_only` control experiment informative
    // rather than trivially empty).
    double referencePrice         = 0.0;

    static NoiseParams fromJson(const nlohmann::json& j);
};

class NoiseTrader : public Strategy {
public:
    void onStart(StrategyContext& ctx) override;
    void onTick(StrategyContext& ctx) override;
    void onFill(StrategyContext& ctx, const Fill& fill) override;
    nlohmann::json metrics() const override;

private:
    void scheduleNextAction(StrategyContext& ctx);
    void maybeCancel(StrategyContext& ctx);
    void submitOne(StrategyContext& ctx);

    NoiseParams params_;
    std::chrono::steady_clock::time_point nextActionAt_{};
    bool warnedNoMarket_ = false;

    // metrics
    long long actions_          = 0;
    long long aggressiveSent_   = 0;
    long long passiveSent_      = 0;
    long long cancelsRequested_ = 0;
    long long fillsObserved_    = 0;
    long long sharesFilled_     = 0;
    long long skippedNoMarket_  = 0;
    long long skippedAtCap_     = 0;
};
