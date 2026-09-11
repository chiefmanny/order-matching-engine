#pragma once

#include "strategy.h"

// Smallest possible real participant, used as the Phase 1 smoke test and as the
// worked example for writing a strategy.
//
// Places one resting bid, optionally crosses it with its own sell so the fill path is
// exercised end-to-end (the engine reports both maker and taker ids, and both are
// ours), then cancels and reports complete.
//
// Parameters: price, size, crossAfterMs (0 = never cross), holdMs.
class EchoStrategy : public Strategy {
public:
    void onStart(StrategyContext& ctx) override;
    void onTick(StrategyContext& ctx) override;
    void onFill(StrategyContext& ctx, const Fill& fill) override;
    nlohmann::json metrics() const override;
    bool isComplete() const override { return complete_; }

private:
    double       price_        = 500.00;
    int          size_         = 100;
    int          crossAfterMs_ = 0;
    int          holdMs_       = 2000;
    unsigned int restingId_    = 0;
    int          fillsObserved_ = 0;
    bool         crossed_      = false;
    bool         complete_     = false;
};
