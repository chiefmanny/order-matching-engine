#pragma once

#include "strategy.h"

#include <chrono>
#include <deque>
#include <string>

// Sustained one-sided pressure: keep sending orders on one side for a while, and see
// whether persistent order-flow imbalance moves the price or is simply absorbed.
//
// Distinct from the two participants it sits between:
//
//   sweep              one-shot, a target quantity, done in a burst. Measures how far
//                      an order reaches into the book RIGHT NOW.
//   directional_trader sustained, a rate rather than a total. Measures whether
//                      repeated pressure moves price PERSISTENTLY, which is a
//                      different question with a different answer -- a book that
//                      absorbs one large order can still be walked by a steady
//                      stream of small ones.
//   noise_trader       both sides, no view, deliberately not directional.
//
// One participant covers buy pressure, sell pressure, order-flow imbalance and
// (with the intensity turned up) the aggressive half of a volatility shock. `side` is
// configuration, so there is no separate bullish and bearish executable to keep in
// step.
struct DirectionalParams {
    std::string side = "BUY";
    int    orderSize          = 200;
    int    arrivalIntervalMs  = 500;
    int    intervalJitterMs   = 0;
    double durationSeconds    = 0.0;   // 0 => until the harness stops it
    long long maxTotalQuantity = 0;    // 0 => unlimited

    // 0.0 rests passively at the near touch; 1.0 crosses to the far touch and trades.
    // Values between interpolate across the spread, so the same participant can be a
    // patient accumulator or an aggressive taker without changing its logic.
    double aggressiveness     = 1.0;
    int    extraCrossTicks    = 0;     // reach beyond the far touch at full aggression

    // Never pay worse than this (above for a buyer, below for a seller). 0 disables.
    // Without it a persistent taker in a thin book can chase price indefinitely.
    double priceLimit         = 0.0;

    // Unfilled remainder of a MARKETABLE order is cancelled after this long, which is
    // the engine's missing immediate-or-cancel approximated. It matters: a buyer whose
    // remainder rests becomes a bid, so an actor meant to CONSUME liquidity would
    // quietly start supplying it and blunt the very pressure being measured.
    // Passive orders (aggressiveness < 1) are left alone -- resting is their purpose.
    int    residualCancelMs   = 1000;

    static DirectionalParams fromJson(const nlohmann::json& j);
    bool isBuy() const { return side == "BUY"; }
};

class DirectionalTrader : public Strategy {
public:
    void onStart(StrategyContext& ctx) override;
    void onTick(StrategyContext& ctx) override;
    void onFill(StrategyContext& ctx, const Fill& fill) override;
    void onStop(StrategyContext& ctx) override;
    nlohmann::json metrics() const override;
    bool isComplete() const override { return complete_; }

private:
    void scheduleNext(StrategyContext& ctx);
    void expireResiduals(StrategyContext& ctx);
    double targetPrice(const StrategyContext& ctx, bool& marketable) const;

    DirectionalParams params_;
    std::chrono::steady_clock::time_point nextOrderAt_{};
    bool complete_ = false;

    // Marketable orders awaiting their residual deadline.
    struct Pending {
        unsigned int orderId;
        std::chrono::steady_clock::time_point expiresAt;
    };
    std::deque<Pending> pending_;

    // metrics
    long long ordersSent_       = 0;
    long long ordersRejected_   = 0;
    long long marketableSent_   = 0;
    long long quantitySent_     = 0;
    long long quantityFilled_   = 0;
    long long residualsCancelled_ = 0;
    long long skippedNoMarket_  = 0;
    long long skippedPriceLimit_ = 0;
    double    notional_         = 0.0;
    double    firstMid_         = 0.0;
    double    lastMid_          = 0.0;
};
