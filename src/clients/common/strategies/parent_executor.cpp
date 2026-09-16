#include "parent_executor.h"

#include "tick_math.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

ParentParams ParentParams::fromJson(const nlohmann::json& j) {
    ParentParams p;
    p.side = j.value("side", p.side);

    // Accept both the parent vocabulary and the legacy sweep vocabulary, so the
    // scenarios written in Phase 6 keep working against this implementation.
    p.parentQuantity = j.value("parentQuantity", j.value("targetQuantity", 0));
    p.mode = j.value("mode", std::string());
    if (p.mode.empty()) {
        // A legacy sweep with childOrders is exactly a fixed-interval slice schedule.
        p.mode = j.value("childOrders", false) ? "sliced" : "immediate";
    }
    p.sliceQuantity  = j.value("sliceQuantity", j.value("childQuantity", 0));
    p.sliceIntervalMs = j.value("sliceIntervalMs", j.value("childDelayMs", p.sliceIntervalMs));

    p.limitPrice = j.value("limitPrice", p.limitPrice);
    p.maxLevels  = j.value("maxLevels", p.maxLevels);
    p.participationRate = j.value("participationRate", p.participationRate);
    p.minSliceQuantity  = j.value("minSliceQuantity", p.minSliceQuantity);
    p.maxSliceQuantity  = j.value("maxSliceQuantity", p.maxSliceQuantity);
    p.horizonSeconds     = j.value("horizonSeconds", p.horizonSeconds);
    p.catchupThreshold   = j.value("catchupThreshold", p.catchupThreshold);
    p.passiveOffsetTicks = j.value("passiveOffsetTicks", p.passiveOffsetTicks);
    p.startDelayMs   = j.value("startDelayMs", p.startDelayMs);
    p.settleMs       = j.value("settleMs", p.settleMs);
    p.maxDurationMs  = j.value("maxDurationMs", p.maxDurationMs);

    if (p.side != "BUY" && p.side != "SELL") {
        throw std::runtime_error("parent_executor: side must be \"BUY\" or \"SELL\"");
    }
    if (p.mode != "immediate" && p.mode != "sliced" &&
        p.mode != "participation" && p.mode != "passive_first") {
        throw std::runtime_error("parent_executor: unknown mode '" + p.mode + "'");
    }
    if (p.parentQuantity <= 0) {
        throw std::runtime_error("parent_executor: parentQuantity must be > 0");
    }
    const bool hasLimit = p.limitPrice > 0.0;
    const bool hasLevels = p.maxLevels > 0;
    if (hasLimit == hasLevels) {
        throw std::runtime_error(
            "parent_executor: specify exactly one of limitPrice or maxLevels");
    }
    if (p.mode == "sliced" && p.sliceQuantity <= 0) {
        throw std::runtime_error("parent_executor: mode 'sliced' requires sliceQuantity");
    }
    if (p.mode == "participation" &&
        (p.participationRate <= 0.0 || p.participationRate > 1.0)) {
        throw std::runtime_error(
            "parent_executor: participationRate must be in (0, 1]");
    }
    if (p.mode == "passive_first" && p.horizonSeconds <= 0.0) {
        throw std::runtime_error("parent_executor: passive_first requires horizonSeconds > 0");
    }
    if (p.sliceIntervalMs < 0 || p.settleMs < 0 || p.startDelayMs < 0) {
        throw std::runtime_error("parent_executor: delays must be >= 0");
    }
    if (p.maxDurationMs <= 0) {
        throw std::runtime_error("parent_executor: maxDurationMs must be > 0");
    }
    return p;
}

void ParentExecutor::onStart(StrategyContext& ctx) {
    params_ = ParentParams::fromJson(ctx.params);
    startedAt_ = ctx.now;
    ctx.log.note("parent_executor: configured",
                 {{"side", params_.side},
                  {"mode", params_.mode},
                  {"parent_quantity", params_.parentQuantity},
                  {"limit_price", params_.limitPrice},
                  {"max_levels", params_.maxLevels}});
}

bool ParentExecutor::arm(StrategyContext& ctx) {
    if (!ctx.market.isValid()) return false;
    const auto& levels = params_.isBuy() ? ctx.market.askLevels : ctx.market.bidLevels;
    if (params_.maxLevels > 0 && levels.empty()) return false;

    arrivalMid_ = ctx.market.midPrice();
    volumeAtArm_ = ctx.market.observedVolume;

    if (params_.limitPrice > 0.0) {
        limitPrice_ = tickmath::round(params_.limitPrice, ctx.tickSize);
    } else {
        const size_t idx =
            std::min(static_cast<size_t>(params_.maxLevels), levels.size()) - 1;
        limitPrice_ = tickmath::round(levels[idx].price, ctx.tickSize);
    }

    armed_ = true;
    armedAt_ = ctx.now;
    lastSliceAt_ = ctx.now - std::chrono::milliseconds(params_.sliceIntervalMs);
    ctx.log.note("parent_executor: armed",
                 {{"arrival_mid", arrivalMid_}, {"limit_price", limitPrice_},
                  {"mode", params_.mode}});
    return true;
}

long long ParentExecutor::nextSliceSize(const StrategyContext& ctx) const {
    const long long remaining = params_.parentQuantity - sentQuantity_;
    if (remaining <= 0) return 0;

    if (params_.mode == "immediate") return remaining;

    if (params_.mode == "sliced") {
        return std::min<long long>(params_.sliceQuantity, remaining);
    }

    if (params_.mode == "participation") {
        // Size against volume the MARKET has printed since we armed, so the order
        // speeds up in a busy market and slows in a quiet one. Our own fills are part
        // of that tape, which is realistic -- a participation algo measures the volume
        // it can see, not some counterfactual without itself in it.
        const long long marketVolume =
            std::max<long long>(0, ctx.market.observedVolume - volumeAtArm_);
        const long long entitled =
            static_cast<long long>(marketVolume * params_.participationRate) - sentQuantity_;
        long long size = std::max<long long>(entitled, 0);
        if (size < params_.minSliceQuantity) {
            // A floor keeps a quiet market from stalling the parent entirely, which
            // would make completion time unbounded rather than merely long.
            size = params_.minSliceQuantity;
        }
        if (params_.maxSliceQuantity > 0) {
            size = std::min<long long>(size, params_.maxSliceQuantity);
        }
        return std::min<long long>(size, remaining);
    }

    // passive_first: keep pace with a linear schedule over the horizon.
    const double elapsed = std::chrono::duration<double>(ctx.now - armedAt_).count();
    const double scheduled =
        std::min(1.0, elapsed / params_.horizonSeconds) * params_.parentQuantity;
    const long long behind = static_cast<long long>(scheduled) - filledQuantity_;
    if (behind <= 0) return 0;
    return std::min<long long>(behind, remaining);
}

double ParentExecutor::slicePrice(const StrategyContext& ctx, bool& marketable) const {
    marketable = true;
    if (params_.mode != "passive_first") return limitPrice_;

    // Rest at the near touch unless the schedule says we are falling behind, in which
    // case cross. This is where the impact/timing trade-off actually bites: resting
    // earns the spread but risks the market leaving without us.
    const double elapsed = std::chrono::duration<double>(ctx.now - armedAt_).count();
    const double scheduled =
        std::min(1.0, elapsed / params_.horizonSeconds) * params_.parentQuantity;
    const double shortfall =
        params_.parentQuantity > 0
            ? (scheduled - filledQuantity_) / params_.parentQuantity
            : 0.0;

    if (shortfall > params_.catchupThreshold) {
        marketable = true;
        return limitPrice_;
    }
    marketable = false;
    const double nearTouch = params_.isBuy() ? ctx.market.bestBid : ctx.market.bestAsk;
    return tickmath::shift(nearTouch,
                           params_.isBuy() ? -params_.passiveOffsetTicks
                                           : params_.passiveOffsetTicks,
                           ctx.tickSize);
}

void ParentExecutor::cancelWorking(StrategyContext& ctx) {
    for (unsigned int id : workingIds_) {
        if (ctx.orders.isLiveOrder(id)) ctx.engine.cancel(id);
    }
    workingIds_.clear();
}

void ParentExecutor::onTick(StrategyContext& ctx) {
    if (complete_) return;

    const auto sinceStart = std::chrono::duration_cast<std::chrono::milliseconds>(
        ctx.now - startedAt_).count();
    if (!armed_) {
        if (sinceStart < params_.startDelayMs) return;
        if (!arm(ctx)) {
            if (sinceStart > params_.maxDurationMs) {
                complete_ = true;
                finishedAt_ = ctx.now;
                ctx.log.note("parent_executor: never armed, no usable market");
            }
            return;
        }
    }

    const auto sinceArmed = std::chrono::duration_cast<std::chrono::milliseconds>(
        ctx.now - armedAt_).count();
    if (sinceArmed > params_.maxDurationMs) {
        ctx.log.note("parent_executor: hit maxDurationMs",
                     {{"filled", filledQuantity_}, {"sent", sentQuantity_}});
        cancelWorking(ctx);
        finishedAt_ = ctx.now;
        complete_ = true;
        return;
    }

    if (filledQuantity_ >= params_.parentQuantity) {
        cancelWorking(ctx);
        finishedAt_ = ctx.now;
        complete_ = true;
        return;
    }

    if (!allSent_) {
        const auto sinceSlice = std::chrono::duration_cast<std::chrono::milliseconds>(
            ctx.now - lastSliceAt_).count();
        const bool due = params_.mode == "immediate" || sinceSlice >= params_.sliceIntervalMs;
        if (due) {
            // passive_first replaces its resting order each time rather than stacking
            // several, so the working quantity always reflects the current shortfall.
            if (params_.mode == "passive_first") cancelWorking(ctx);

            const long long size = nextSliceSize(ctx);
            if (size > 0) {
                bool marketable = false;
                const double price = slicePrice(ctx, marketable);
                if (price > 0.0) {
                    const unsigned int id = ctx.engine.submit(
                        params_.isBuy(), price, static_cast<int>(size));
                    if (id != 0) {
                        workingIds_.push_back(id);
                        ++sliceCount_;
                        if (marketable) ++aggressiveSlices_;
                        // passive_first only counts quantity as "sent" once it is
                        // actually working aggressively; a resting order that is
                        // cancelled and replaced must not consume the parent budget.
                        if (params_.mode != "passive_first") sentQuantity_ += size;
                        if (marketable && params_.mode == "passive_first") ++catchupEvents_;
                    }
                }
            }
            lastSliceAt_ = ctx.now;
            if (params_.mode != "passive_first" && sentQuantity_ >= params_.parentQuantity) {
                allSent_ = true;
                allSentAt_ = ctx.now;
            }
        }
    }

    if (allSent_) {
        const auto sinceAllSent = std::chrono::duration_cast<std::chrono::milliseconds>(
            ctx.now - allSentAt_).count();
        if (sinceAllSent >= params_.settleMs) {
            cancelWorking(ctx);
            finishedAt_ = ctx.now;
            complete_ = true;
        }
    }
}

void ParentExecutor::onFill(StrategyContext& ctx, const Fill& fill) {
    filledQuantity_ += fill.size;
    notional_ += fill.price * fill.size;
}

void ParentExecutor::onStop(StrategyContext& ctx) {
    cancelWorking(ctx);
    if (finishedAt_.time_since_epoch().count() == 0) finishedAt_ = ctx.now;
}

nlohmann::json ParentExecutor::metrics() const {
    const double vwap = filledQuantity_ > 0 ? notional_ / filledQuantity_ : 0.0;
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
        {"mode", params_.mode},
        {"side", params_.side},
        {"armed", armed_},
        {"parent_quantity", params_.parentQuantity},
        // Legacy sweep metric names are kept so Phase 6 analysis keeps resolving.
        {"target_quantity", params_.parentQuantity},
        {"sent_quantity", sentQuantity_},
        {"filled_quantity", filledQuantity_},
        {"fill_completion", params_.parentQuantity > 0
             ? static_cast<double>(filledQuantity_) / params_.parentQuantity : 0.0},
        {"slices", sliceCount_},
        {"child_orders", sliceCount_},
        {"aggressive_slices", aggressiveSlices_},
        {"catchup_events", catchupEvents_},
        {"arrival_mid", arrivalMid_},
        {"limit_price", limitPrice_},
        {"vwap", vwap},
        {"slippage_bps", slippageBps},
        {"duration_ms", durationMs},
        {"completion_seconds", durationMs / 1000.0},
    };
}
