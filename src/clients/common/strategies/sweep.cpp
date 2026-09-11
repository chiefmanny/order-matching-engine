#include "sweep.h"

#include "tick_math.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

SweepParams SweepParams::fromJson(const nlohmann::json& j) {
    SweepParams p;
    p.side           = j.value("side", p.side);
    p.targetQuantity = j.value("targetQuantity", p.targetQuantity);
    p.limitPrice     = j.value("limitPrice", p.limitPrice);
    p.maxLevels      = j.value("maxLevels", p.maxLevels);
    p.childOrders    = j.value("childOrders", p.childOrders);
    p.childQuantity  = j.value("childQuantity", p.childQuantity);
    p.childDelayMs   = j.value("childDelayMs", p.childDelayMs);
    p.startDelayMs   = j.value("startDelayMs", p.startDelayMs);
    p.settleMs       = j.value("settleMs", p.settleMs);
    p.maxDurationMs  = j.value("maxDurationMs", p.maxDurationMs);

    if (p.side != "BUY" && p.side != "SELL") {
        throw std::runtime_error("sweep: side must be \"BUY\" or \"SELL\"");
    }
    if (p.targetQuantity <= 0) {
        throw std::runtime_error("sweep: targetQuantity must be > 0");
    }
    const bool hasLimit = p.limitPrice > 0.0;
    const bool hasLevels = p.maxLevels > 0;
    if (hasLimit == hasLevels) {
        // Requiring exactly one avoids the silent ambiguity of "which wins?" -- and a
        // sweep with neither would reach arbitrarily far into the book.
        throw std::runtime_error(
            "sweep: specify exactly one of limitPrice or maxLevels");
    }
    if (p.childOrders && p.childQuantity <= 0) {
        throw std::runtime_error("sweep: childOrders requires childQuantity > 0");
    }
    if (p.childDelayMs < 0 || p.settleMs < 0 || p.startDelayMs < 0) {
        throw std::runtime_error("sweep: delays must be >= 0");
    }
    if (p.maxDurationMs <= 0) {
        throw std::runtime_error("sweep: maxDurationMs must be > 0");
    }
    return p;
}

void Sweep::onStart(StrategyContext& ctx) {
    params_ = SweepParams::fromJson(ctx.params);
    startedAt_ = ctx.now;
    ctx.log.note("sweep: configured",
                 {{"side", params_.side},
                  {"target_quantity", params_.targetQuantity},
                  {"max_levels", params_.maxLevels},
                  {"limit_price", params_.limitPrice},
                  {"child_orders", params_.childOrders},
                  {"child_quantity", params_.childQuantity}});
}

bool Sweep::arm(StrategyContext& ctx) {
    if (!ctx.market.isValid()) return false;

    const auto& levels = params_.isBuy() ? ctx.market.askLevels : ctx.market.bidLevels;
    if (params_.maxLevels > 0 && levels.empty()) {
        // The Level 2 poller has not delivered a snapshot yet; without it we cannot
        // convert maxLevels into a price, and guessing would silently change the
        // experiment's reach into the book.
        return false;
    }

    arrivalBestBid_ = ctx.market.bestBid;
    arrivalBestAsk_ = ctx.market.bestAsk;
    arrivalMid_     = ctx.market.midPrice();

    if (params_.limitPrice > 0.0) {
        limitPrice_ = tickmath::round(params_.limitPrice, ctx.tickSize);
    } else {
        const size_t idx = std::min(static_cast<size_t>(params_.maxLevels),
                                    levels.size()) - 1;
        limitPrice_ = tickmath::round(levels[idx].price, ctx.tickSize);
    }

    // Record how much was actually showing within our limit at arrival. This is the
    // liquidity the sweep *expected* to find, and comparing it against what filled is
    // what makes "the book was thinner than displayed" a measurable claim.
    for (const auto& level : levels) {
        const bool within = params_.isBuy() ? level.price <= limitPrice_ + 1e-9
                                            : level.price >= limitPrice_ - 1e-9;
        if (!within) continue;
        arrivalDepthWithinLimit_ += level.size;
        ++arrivalLevelsWithinLimit_;
    }

    armed_ = true;
    armedAt_ = ctx.now;
    lastChildAt_ = ctx.now - std::chrono::milliseconds(params_.childDelayMs);

    ctx.log.note("sweep: armed",
                 {{"arrival_best_bid", arrivalBestBid_},
                  {"arrival_best_ask", arrivalBestAsk_},
                  {"arrival_mid", arrivalMid_},
                  {"limit_price", limitPrice_},
                  {"arrival_depth_within_limit", arrivalDepthWithinLimit_},
                  {"arrival_levels_within_limit", arrivalLevelsWithinLimit_}});
    return true;
}

void Sweep::cancelResidual(StrategyContext& ctx) {
    if (residualCancelled_) return;
    residualCancelled_ = true;
    int cancelled = 0;
    for (unsigned int id : childIds_) {
        if (ctx.orders.isLiveOrder(id)) {
            ctx.engine.cancel(id);
            ++cancelled;
        }
    }
    if (cancelled > 0) {
        // Unfilled remainder would otherwise rest inside the book as new liquidity at
        // an aggressive price, contaminating every post-sweep measurement.
        ctx.log.note("sweep: cancelled unfilled residual", {{"orders", cancelled}});
    }
}

void Sweep::onTick(StrategyContext& ctx) {
    if (complete_) return;

    const auto sinceStart = std::chrono::duration_cast<std::chrono::milliseconds>(
        ctx.now - startedAt_).count();

    if (!armed_) {
        if (sinceStart < params_.startDelayMs) return;
        if (!arm(ctx)) {
            if (sinceStart > params_.maxDurationMs) {
                ctx.log.note("sweep: never armed, no usable market");
                complete_ = true;
                finishedAt_ = ctx.now;
            }
            return;
        }
    }

    const auto sinceArmed = std::chrono::duration_cast<std::chrono::milliseconds>(
        ctx.now - armedAt_).count();
    if (sinceArmed > params_.maxDurationMs) {
        ctx.log.note("sweep: hit maxDurationMs", {{"filled", filledQuantity_}});
        cancelResidual(ctx);
        finishedAt_ = ctx.now;
        complete_ = true;
        return;
    }

    // --- send ----------------------------------------------------------
    if (!allSent_) {
        const long long remaining = params_.targetQuantity - sentQuantity_;
        if (remaining <= 0) {
            allSent_ = true;
            allSentAt_ = ctx.now;
        } else {
            const auto sinceChild = std::chrono::duration_cast<std::chrono::milliseconds>(
                ctx.now - lastChildAt_).count();
            const bool due = !params_.childOrders || sinceChild >= params_.childDelayMs;
            if (due) {
                // Clamp to the remaining target: the sent quantity is what bounds
                // execution, so this is the invariant that keeps fills <= target.
                const long long size = params_.childOrders
                    ? std::min<long long>(params_.childQuantity, remaining)
                    : remaining;
                const unsigned int id =
                    ctx.engine.submit(params_.isBuy(), limitPrice_, static_cast<int>(size));
                if (id != 0) {
                    childIds_.push_back(id);
                    sentQuantity_ += size;
                    ++childCount_;
                } else {
                    ++rejectedChildren_;
                }
                lastChildAt_ = ctx.now;
                if (sentQuantity_ >= params_.targetQuantity) {
                    allSent_ = true;
                    allSentAt_ = ctx.now;
                }
            }
        }
    }

    // --- settle --------------------------------------------------------
    if (allSent_) {
        if (filledQuantity_ >= params_.targetQuantity) {
            finishedAt_ = ctx.now;
            cancelResidual(ctx);
            complete_ = true;
            return;
        }
        const auto sinceAllSent = std::chrono::duration_cast<std::chrono::milliseconds>(
            ctx.now - allSentAt_).count();
        if (sinceAllSent >= params_.settleMs) {
            // Everything is in the market and has had its chance to trade; anything
            // still resting is liquidity we are adding, not taking. Pull it.
            finishedAt_ = ctx.now;
            cancelResidual(ctx);
            complete_ = true;
        }
    }
}

void Sweep::onFill(StrategyContext& ctx, const Fill& fill) {
    filledQuantity_ += fill.size;
    notional_ += fill.price * fill.size;
}

void Sweep::onStop(StrategyContext& ctx) {
    cancelResidual(ctx);
    if (finishedAt_.time_since_epoch().count() == 0) finishedAt_ = ctx.now;
}

nlohmann::json Sweep::metrics() const {
    const double vwap = filledQuantity_ > 0 ? notional_ / filledQuantity_ : 0.0;

    // Slippage against the arrival mid, signed so positive always means "worse for
    // the sweeper" on both sides. Basis points keeps it comparable across prices.
    double slippageBps = 0.0;
    if (vwap > 0.0 && arrivalMid_ > 0.0) {
        const double diff = params_.isBuy() ? (vwap - arrivalMid_) : (arrivalMid_ - vwap);
        slippageBps = diff / arrivalMid_ * 1e4;
    }

    double durationMs = 0.0;
    if (armed_ && finishedAt_.time_since_epoch().count() != 0) {
        durationMs = std::chrono::duration<double, std::milli>(finishedAt_ - armedAt_).count();
    }

    return {
        {"side", params_.side},
        {"armed", armed_},
        {"target_quantity", params_.targetQuantity},
        {"sent_quantity", sentQuantity_},
        {"filled_quantity", filledQuantity_},
        {"fill_completion", params_.targetQuantity > 0
                                ? static_cast<double>(filledQuantity_) / params_.targetQuantity
                                : 0.0},
        {"child_orders", childCount_},
        {"rejected_children", rejectedChildren_},
        {"arrival_best_bid", arrivalBestBid_},
        {"arrival_best_ask", arrivalBestAsk_},
        {"arrival_mid", arrivalMid_},
        {"limit_price", limitPrice_},
        {"arrival_depth_within_limit", arrivalDepthWithinLimit_},
        {"arrival_levels_within_limit", arrivalLevelsWithinLimit_},
        {"vwap", vwap},
        {"slippage_bps", slippageBps},
        {"duration_ms", durationMs},
        {"residual_cancelled", residualCancelled_},
    };
}
