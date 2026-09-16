#pragma once

#include "strategy.h"

#include <chrono>
#include <string>
#include <vector>

// A large trader working a parent quantity, in one of several execution styles.
//
// This is the generalisation of the Phase 6 sweep rather than a second executor:
// `sweep` is registered as an alias for mode "immediate", and the legacy sweep
// parameters (childOrders / childQuantity / childDelayMs) still resolve to mode
// "sliced", so every existing sweep scenario keeps working unchanged.
//
// The point of the modes is that they trade the SAME quantity against the SAME book
// and get different prices, which is the entire premise of execution algorithms:
//
//   immediate      all at once. Fastest, and pays the most impact.
//   sliced         fixed child size on a fixed interval (a TWAP). Gives the book time
//                  to replenish between children, at the cost of exposure to drift
//                  while the order is still working.
//   participation  child size tracks realised market volume (a POV). Trades more when
//                  the market is busy and less when it is quiet, so it hides in the
//                  flow instead of imposing on it.
//   passive_first  rest at the near touch and earn the spread rather than pay it,
//                  crossing only when the schedule says it is falling behind.
//
// Almgren-Chriss is the motivation, not the implementation: the trade-off between
// market impact and timing risk is exactly what separates these modes, but there is
// no volatility estimate, no risk-aversion parameter and no optimal trajectory here.
// Which mode wins is an experimental question.
struct ParentParams {
    std::string side = "BUY";
    std::string mode = "immediate";   // immediate | sliced | participation | passive_first
    int    parentQuantity = 0;        // required, > 0

    // Reach into the book: exactly one of these.
    double limitPrice = 0.0;
    int    maxLevels  = 0;

    // sliced
    int    sliceQuantity  = 0;
    int    sliceIntervalMs = 200;

    // participation
    double participationRate = 0.2;   // fraction of observed market volume to take
    int    minSliceQuantity  = 50;    // floor, so a quiet market does not stall it
    int    maxSliceQuantity  = 0;     // 0 => no cap

    // passive_first
    double horizonSeconds     = 20.0; // schedule over which the parent should complete
    double catchupThreshold   = 0.15; // cross once this far behind schedule (fraction)
    int    passiveOffsetTicks = 0;    // rest this far behind the near touch

    int    startDelayMs   = 0;
    int    settleMs       = 400;      // wait for fills after the last child, then cancel
    int    maxDurationMs  = 60000;

    static ParentParams fromJson(const nlohmann::json& j);
    bool isBuy() const { return side == "BUY"; }
};

class ParentExecutor : public Strategy {
public:
    void onStart(StrategyContext& ctx) override;
    void onTick(StrategyContext& ctx) override;
    void onFill(StrategyContext& ctx, const Fill& fill) override;
    void onStop(StrategyContext& ctx) override;
    nlohmann::json metrics() const override;
    bool isComplete() const override { return complete_; }

private:
    bool arm(StrategyContext& ctx);
    void cancelWorking(StrategyContext& ctx);
    // How much to send now, and at what price. Returns 0 to send nothing this tick.
    long long nextSliceSize(const StrategyContext& ctx) const;
    double slicePrice(const StrategyContext& ctx, bool& marketable) const;

    ParentParams params_;
    bool armed_ = false;
    bool complete_ = false;
    bool allSent_ = false;

    std::chrono::steady_clock::time_point startedAt_{};
    std::chrono::steady_clock::time_point armedAt_{};
    std::chrono::steady_clock::time_point lastSliceAt_{};
    std::chrono::steady_clock::time_point allSentAt_{};
    std::chrono::steady_clock::time_point finishedAt_{};

    double arrivalMid_ = 0.0;
    double limitPrice_ = 0.0;
    long long volumeAtArm_ = 0;

    long long sentQuantity_ = 0;
    long long filledQuantity_ = 0;
    double    notional_ = 0.0;
    int       sliceCount_ = 0;
    int       aggressiveSlices_ = 0;
    int       catchupEvents_ = 0;
    std::vector<unsigned int> workingIds_;
};
