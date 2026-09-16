#pragma once

#include "strategy.h"

#include <chrono>
#include <string>
#include <vector>

// An educational demonstration of layering, for surveillance metrics to be measured
// against in this simulator.
//
// SCOPE. This exists so that detection metrics have something to detect. Layering is
// a long-documented and publicly described pattern, and the interesting content here
// is not the pattern itself -- it is the behavioural fingerprint it leaves, which is
// what surveillance systems actually key on:
//
//     cancel ratio        ~100%   almost nothing it posts is meant to trade
//     fill ratio          ~0%     it withdraws before it can be hit
//     order lifetime      sub-second, and strikingly uniform
//     displayed size      far larger than any genuine participant's
//     order-to-trade      enormous, because the orders are the point
//
// Every one of those is visible in metrics/manipulation.py alongside the genuine
// participants, and the educational payload is the contrast: the honest actors and
// this one separate cleanly on a scatter of cancel ratio against order lifetime.
//
// It runs only against this synthetic engine with simulated participants, and is
// deliberately naive -- a fixed cadence, uniform layer sizes, a constant offset from
// the touch. Those are exactly the regularities that make it easy to spot, and they
// are left in on purpose.
struct LayeringParams {
    std::string side = "BUY";     // side the displayed orders are placed on
    int layers            = 5;
    int layerSize         = 2000; // deliberately large next to genuine participants
    int startOffsetTicks  = 2;    // first layer this far behind the touch
    int layerSpacingTicks = 1;
    int holdMs            = 800;  // how long layers rest before being pulled
    int cycleIntervalMs   = 2000; // gap between cycles
    int cycles            = 0;    // 0 => until the harness stops it

    // Optionally trade small size on the OPPOSITE side while the layers are showing.
    // This is what makes the pattern worth demonstrating rather than merely odd: the
    // displayed orders are not the trade, they are cover for one going the other way.
    bool tradeOpposite = false;
    int  opposingSize  = 200;

    static LayeringParams fromJson(const nlohmann::json& j);
    bool isBuy() const { return side == "BUY"; }
};

class LayeringDemo : public Strategy {
public:
    void onStart(StrategyContext& ctx) override;
    void onTick(StrategyContext& ctx) override;
    void onFill(StrategyContext& ctx, const Fill& fill) override;
    void onStop(StrategyContext& ctx) override;
    nlohmann::json metrics() const override;
    bool isComplete() const override { return complete_; }

private:
    enum class Phase { Waiting, Displayed };

    void placeLayers(StrategyContext& ctx);
    void pullLayers(StrategyContext& ctx);

    LayeringParams params_;
    Phase phase_ = Phase::Waiting;
    bool complete_ = false;
    std::chrono::steady_clock::time_point phaseStartedAt_{};
    std::vector<unsigned int> layerIds_;

    long long cyclesCompleted_ = 0;
    long long layersPlaced_    = 0;
    long long layersCancelled_ = 0;
    long long layersFilled_    = 0;   // should stay near zero; a non-zero value means
                                      // the actor was caught before it could withdraw
    long long displayedQuantity_ = 0;
    long long opposingTrades_  = 0;
    long long opposingFilled_  = 0;
};
