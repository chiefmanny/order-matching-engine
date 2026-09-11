#include "noise_trader.h"

#include "tick_math.h"

#include <algorithm>
#include <stdexcept>

NoiseParams NoiseParams::fromJson(const nlohmann::json& j) {
    NoiseParams p;
    p.orderIntervalMs       = j.value("orderIntervalMs", p.orderIntervalMs);
    p.intervalJitterMs      = j.value("intervalJitterMs", p.intervalJitterMs);
    p.meanQuantity          = j.value("meanQuantity", p.meanQuantity);
    p.quantityVariance      = j.value("quantityVariance", p.quantityVariance);
    p.aggressiveProbability = j.value("aggressiveProbability", p.aggressiveProbability);
    p.cancelProbability     = j.value("cancelProbability", p.cancelProbability);
    p.buyProbability        = j.value("buyProbability", p.buyProbability);
    p.passivePlacementTicks = j.value("passivePlacementTicks", p.passivePlacementTicks);
    p.aggressiveCrossTicks  = j.value("aggressiveCrossTicks", p.aggressiveCrossTicks);
    p.maxOpenOrders         = j.value("maxOpenOrders", p.maxOpenOrders);
    p.referencePrice        = j.value("referencePrice", p.referencePrice);

    if (p.orderIntervalMs < 1) {
        throw std::runtime_error("noise_trader: orderIntervalMs must be >= 1");
    }
    if (p.intervalJitterMs < 0 || p.intervalJitterMs >= p.orderIntervalMs) {
        throw std::runtime_error(
            "noise_trader: intervalJitterMs must be >= 0 and < orderIntervalMs");
    }
    if (p.meanQuantity < 1) {
        throw std::runtime_error("noise_trader: meanQuantity must be >= 1");
    }
    if (p.quantityVariance < 0 || p.quantityVariance >= p.meanQuantity) {
        throw std::runtime_error(
            "noise_trader: quantityVariance must be >= 0 and < meanQuantity");
    }
    auto probability = [](double v, const char* name) {
        if (v < 0.0 || v > 1.0) {
            throw std::runtime_error(std::string("noise_trader: ") + name +
                                     " must be between 0 and 1");
        }
    };
    probability(p.aggressiveProbability, "aggressiveProbability");
    probability(p.cancelProbability, "cancelProbability");
    probability(p.buyProbability, "buyProbability");
    if (p.passivePlacementTicks < 0) {
        throw std::runtime_error("noise_trader: passivePlacementTicks must be >= 0");
    }
    if (p.aggressiveCrossTicks < 0) {
        throw std::runtime_error("noise_trader: aggressiveCrossTicks must be >= 0");
    }
    if (p.maxOpenOrders < 1) {
        throw std::runtime_error("noise_trader: maxOpenOrders must be >= 1");
    }
    return p;
}

void NoiseTrader::onStart(StrategyContext& ctx) {
    params_ = NoiseParams::fromJson(ctx.params);
    scheduleNextAction(ctx);
    ctx.log.note("noise_trader: configured",
                 {{"order_interval_ms", params_.orderIntervalMs},
                  {"aggressive_probability", params_.aggressiveProbability},
                  {"cancel_probability", params_.cancelProbability},
                  {"mean_quantity", params_.meanQuantity}});
}

void NoiseTrader::scheduleNextAction(StrategyContext& ctx) {
    const int jitter = params_.intervalJitterMs > 0
                           ? ctx.rng.intInRange(-params_.intervalJitterMs,
                                                params_.intervalJitterMs)
                           : 0;
    const int gap = std::max(1, params_.orderIntervalMs + jitter);
    nextActionAt_ = ctx.now + std::chrono::milliseconds(gap);
}

void NoiseTrader::maybeCancel(StrategyContext& ctx) {
    if (!ctx.rng.chance(params_.cancelProbability)) return;
    auto live = ctx.orders.activeOrderIds();
    if (live.empty()) return;
    const int index = ctx.rng.intInRange(0, static_cast<int>(live.size()) - 1);
    ctx.engine.cancel(live[index]);
    ++cancelsRequested_;
}

void NoiseTrader::submitOne(StrategyContext& ctx) {
    // Resting orders are capped so background flow cannot slowly accumulate into a
    // book of its own and start competing with the liquidity provider.
    if (ctx.orders.activeCount() >= params_.maxOpenOrders) {
        ++skippedAtCap_;
        return;
    }

    const bool buy = ctx.rng.chance(params_.buyProbability);
    const int size = params_.quantityVariance > 0
                         ? ctx.rng.sizeAround(params_.meanQuantity, params_.quantityVariance)
                         : params_.meanQuantity;

    const bool twoSided = ctx.market.isValid();
    double price = 0.0;

    if (!twoSided) {
        // Bootstrap mode: with no two-sided book there is no touch to reference, so
        // quote passively around the configured reference. Without this the trader
        // would sit idle forever whenever no liquidity provider is running.
        if (!(params_.referencePrice > 0.0)) {
            ++skippedNoMarket_;
            if (!warnedNoMarket_) {
                ctx.log.note("noise_trader: idle, no two-sided market and no referencePrice");
                warnedNoMarket_ = true;
            }
            return;
        }
        const int offset = ctx.rng.intInRange(1, std::max(1, params_.passivePlacementTicks));
        price = tickmath::shift(params_.referencePrice, buy ? -offset : offset, ctx.tickSize);
        ++passiveSent_;
    } else if (ctx.rng.chance(params_.aggressiveProbability)) {
        // Aggressive: cross to the far touch. The engine has no market order type, so
        // this is a marketable limit -- whatever does not execute rests at that price,
        // which is exactly how a real marketable limit behaves.
        const double farTouch = buy ? ctx.market.bestAsk : ctx.market.bestBid;
        price = tickmath::shift(farTouch,
                                buy ? params_.aggressiveCrossTicks : -params_.aggressiveCrossTicks,
                                ctx.tickSize);
        ++aggressiveSent_;
    } else {
        // Passive: join or sit behind the near touch. Never inside the spread -- that
        // is the market maker's job, and letting noise tighten the spread would blur
        // the very effect Phase 8 sets out to measure.
        const double nearTouch = buy ? ctx.market.bestBid : ctx.market.bestAsk;
        const int offset = params_.passivePlacementTicks > 0
                               ? ctx.rng.intInRange(0, params_.passivePlacementTicks)
                               : 0;
        price = tickmath::shift(nearTouch, buy ? -offset : offset, ctx.tickSize);
        ++passiveSent_;
    }

    if (!(price > 0.0)) return;
    ctx.engine.submit(buy, price, size);
}

void NoiseTrader::onTick(StrategyContext& ctx) {
    if (ctx.now < nextActionAt_) return;
    scheduleNextAction(ctx);
    ++actions_;

    // Cancel and submit are independent draws, so churn does not come at the cost of
    // arrival rate.
    maybeCancel(ctx);
    submitOne(ctx);
}

void NoiseTrader::onFill(StrategyContext& ctx, const Fill& fill) {
    ++fillsObserved_;
    sharesFilled_ += fill.size;
}

nlohmann::json NoiseTrader::metrics() const {
    const long long sent = aggressiveSent_ + passiveSent_;
    return {
        {"actions", actions_},
        {"orders_sent", sent},
        {"aggressive_sent", aggressiveSent_},
        {"passive_sent", passiveSent_},
        {"aggressive_share", sent > 0 ? static_cast<double>(aggressiveSent_) / sent : 0.0},
        {"cancels_requested", cancelsRequested_},
        {"fills_observed", fillsObserved_},
        {"shares_filled", sharesFilled_},
        {"skipped_no_market", skippedNoMarket_},
        {"skipped_at_open_order_cap", skippedAtCap_},
    };
}
