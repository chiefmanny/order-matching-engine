#include "directional_trader.h"

#include "tick_math.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

DirectionalParams DirectionalParams::fromJson(const nlohmann::json& j) {
    DirectionalParams p;
    p.side               = j.value("side", p.side);
    p.orderSize          = j.value("orderSize", p.orderSize);
    p.arrivalIntervalMs  = j.value("arrivalIntervalMs", p.arrivalIntervalMs);
    p.intervalJitterMs   = j.value("intervalJitterMs", p.intervalJitterMs);
    p.durationSeconds    = j.value("durationSeconds", p.durationSeconds);
    p.maxTotalQuantity   = j.value("maxTotalQuantity", p.maxTotalQuantity);
    p.aggressiveness     = j.value("aggressiveness", p.aggressiveness);
    p.extraCrossTicks    = j.value("extraCrossTicks", p.extraCrossTicks);
    p.priceLimit         = j.value("priceLimit", p.priceLimit);
    p.residualCancelMs   = j.value("residualCancelMs", p.residualCancelMs);

    if (p.side != "BUY" && p.side != "SELL") {
        throw std::runtime_error("directional_trader: side must be \"BUY\" or \"SELL\"");
    }
    if (p.orderSize < 1) {
        throw std::runtime_error("directional_trader: orderSize must be >= 1");
    }
    if (p.arrivalIntervalMs < 1) {
        throw std::runtime_error("directional_trader: arrivalIntervalMs must be >= 1");
    }
    if (p.intervalJitterMs < 0 || p.intervalJitterMs >= p.arrivalIntervalMs) {
        throw std::runtime_error(
            "directional_trader: intervalJitterMs must be >= 0 and < arrivalIntervalMs");
    }
    if (p.aggressiveness < 0.0 || p.aggressiveness > 1.0) {
        throw std::runtime_error(
            "directional_trader: aggressiveness must be between 0 and 1");
    }
    if (p.extraCrossTicks < 0) {
        throw std::runtime_error("directional_trader: extraCrossTicks must be >= 0");
    }
    if (p.durationSeconds < 0.0) {
        throw std::runtime_error("directional_trader: durationSeconds must be >= 0");
    }
    if (p.maxTotalQuantity < 0) {
        throw std::runtime_error("directional_trader: maxTotalQuantity must be >= 0");
    }
    if (p.residualCancelMs < 0) {
        throw std::runtime_error("directional_trader: residualCancelMs must be >= 0");
    }
    return p;
}

void DirectionalTrader::onStart(StrategyContext& ctx) {
    params_ = DirectionalParams::fromJson(ctx.params);
    scheduleNext(ctx);
    ctx.log.note("directional_trader: configured",
                 {{"side", params_.side},
                  {"order_size", params_.orderSize},
                  {"arrival_interval_ms", params_.arrivalIntervalMs},
                  {"aggressiveness", params_.aggressiveness},
                  {"duration_seconds", params_.durationSeconds},
                  {"max_total_quantity", params_.maxTotalQuantity}});
}

void DirectionalTrader::scheduleNext(StrategyContext& ctx) {
    const int jitter = params_.intervalJitterMs > 0
                           ? ctx.rng.intInRange(-params_.intervalJitterMs,
                                                params_.intervalJitterMs)
                           : 0;
    nextOrderAt_ = ctx.now +
                   std::chrono::milliseconds(std::max(1, params_.arrivalIntervalMs + jitter));
}

double DirectionalTrader::targetPrice(const StrategyContext& ctx, bool& marketable) const {
    marketable = false;
    if (!ctx.market.isValid()) return 0.0;

    const double near = params_.isBuy() ? ctx.market.bestBid : ctx.market.bestAsk;
    const double far  = params_.isBuy() ? ctx.market.bestAsk : ctx.market.bestBid;

    // Interpolate across the spread: 0 rests at the near touch, 1 reaches the far one.
    double price = near + params_.aggressiveness * (far - near);
    price = tickmath::round(price, ctx.tickSize);

    const bool reachesFar = params_.isBuy() ? price >= far - 1e-9 : price <= far + 1e-9;
    if (reachesFar) {
        marketable = true;
        if (params_.extraCrossTicks > 0) {
            price = tickmath::shift(far,
                                    params_.isBuy() ? params_.extraCrossTicks
                                                    : -params_.extraCrossTicks,
                                    ctx.tickSize);
        }
    }
    return price;
}

void DirectionalTrader::expireResiduals(StrategyContext& ctx) {
    while (!pending_.empty() && pending_.front().expiresAt <= ctx.now) {
        const auto entry = pending_.front();
        pending_.pop_front();
        if (ctx.orders.isLiveOrder(entry.orderId)) {
            ctx.engine.cancel(entry.orderId);
            ++residualsCancelled_;
        }
    }
}

void DirectionalTrader::onTick(StrategyContext& ctx) {
    if (complete_) return;

    expireResiduals(ctx);

    if (ctx.market.isValid()) {
        const double mid = ctx.market.midPrice();
        if (firstMid_ <= 0.0) firstMid_ = mid;
        lastMid_ = mid;
    }

    if (params_.durationSeconds > 0.0 && ctx.elapsedSeconds >= params_.durationSeconds) {
        // Pull any residual before finishing, so the runtime's shutdown is not what
        // removes this participant's last footprint from the book.
        for (const auto& entry : pending_) {
            if (ctx.orders.isLiveOrder(entry.orderId)) {
                ctx.engine.cancel(entry.orderId);
                ++residualsCancelled_;
            }
        }
        pending_.clear();
        complete_ = true;
        ctx.log.note("directional_trader: duration reached",
                     {{"quantity_sent", quantitySent_}, {"quantity_filled", quantityFilled_}});
        return;
    }

    if (params_.maxTotalQuantity > 0 && quantitySent_ >= params_.maxTotalQuantity) {
        complete_ = true;
        ctx.log.note("directional_trader: quantity budget exhausted",
                     {{"quantity_sent", quantitySent_}});
        return;
    }

    if (ctx.now < nextOrderAt_) return;
    scheduleNext(ctx);

    bool marketable = false;
    const double price = targetPrice(ctx, marketable);
    if (!(price > 0.0)) {
        ++skippedNoMarket_;
        return;
    }

    // Respect the price limit rather than chasing. A persistent taker that ignores it
    // will walk a thin book arbitrarily far, which measures the limit's absence
    // rather than anything about the market.
    if (params_.priceLimit > 0.0) {
        const bool tooFar = params_.isBuy() ? price > params_.priceLimit + 1e-9
                                            : price < params_.priceLimit - 1e-9;
        if (tooFar) {
            ++skippedPriceLimit_;
            return;
        }
    }

    long long size = params_.orderSize;
    if (params_.maxTotalQuantity > 0) {
        size = std::min<long long>(size, params_.maxTotalQuantity - quantitySent_);
    }
    if (size <= 0) return;

    const unsigned int id =
        ctx.engine.submit(params_.isBuy(), price, static_cast<int>(size));
    if (id == 0) {
        ++ordersRejected_;
        return;
    }
    ++ordersSent_;
    quantitySent_ += size;
    if (marketable) {
        ++marketableSent_;
        if (params_.residualCancelMs > 0) {
            pending_.push_back(
                Pending{id, ctx.now + std::chrono::milliseconds(params_.residualCancelMs)});
        }
    }
}

void DirectionalTrader::onFill(StrategyContext& ctx, const Fill& fill) {
    quantityFilled_ += fill.size;
    notional_ += fill.price * fill.size;
}

void DirectionalTrader::onStop(StrategyContext& ctx) {
    for (const auto& entry : pending_) {
        if (ctx.orders.isLiveOrder(entry.orderId)) {
            ctx.engine.cancel(entry.orderId);
            ++residualsCancelled_;
        }
    }
    pending_.clear();
}

nlohmann::json DirectionalTrader::metrics() const {
    const double vwap = quantityFilled_ > 0 ? notional_ / quantityFilled_ : 0.0;
    return {
        {"side", params_.side},
        {"orders_sent", ordersSent_},
        {"orders_rejected", ordersRejected_},
        {"marketable_sent", marketableSent_},
        {"marketable_share",
             ordersSent_ > 0 ? static_cast<double>(marketableSent_) / ordersSent_ : 0.0},
        {"quantity_sent", quantitySent_},
        {"quantity_filled", quantityFilled_},
        {"fill_ratio",
             quantitySent_ > 0 ? static_cast<double>(quantityFilled_) / quantitySent_ : 0.0},
        {"vwap", vwap},
        {"residuals_cancelled", residualsCancelled_},
        {"skipped_no_market", skippedNoMarket_},
        {"skipped_price_limit", skippedPriceLimit_},
        {"first_mid", firstMid_},
        {"last_mid", lastMid_},
        {"mid_move", lastMid_ - firstMid_},
    };
}
