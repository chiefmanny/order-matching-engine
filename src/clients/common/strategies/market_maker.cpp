#include "market_maker.h"

#include "tick_math.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

MarketMakerParams MarketMakerParams::fromJson(const nlohmann::json& j) {
    MarketMakerParams p;
    p.quoteSize             = j.value("quoteSize", p.quoteSize);
    p.halfSpreadTicks       = j.value("halfSpreadTicks", p.halfSpreadTicks);
    p.requoteThresholdTicks = j.value("requoteThresholdTicks", p.requoteThresholdTicks);
    p.requoteIntervalMs     = j.value("requoteIntervalMs", p.requoteIntervalMs);
    p.inventoryTarget       = j.value("inventoryTarget", p.inventoryTarget);
    p.inventoryLimit        = j.value("inventoryLimit", p.inventoryLimit);
    p.inventoryUnitShares   = j.value("inventoryUnitShares", p.inventoryUnitShares);
    p.inventorySkewTicksPerUnit =
        j.value("inventorySkewTicksPerUnit", p.inventorySkewTicksPerUnit);
    p.maxInventorySkewTicks = j.value("maxInventorySkewTicks", p.maxInventorySkewTicks);
    p.referenceMode         = j.value("referenceMode", p.referenceMode);

    if (p.quoteSize < 1) {
        throw std::runtime_error("market_maker: quoteSize must be >= 1");
    }
    if (p.halfSpreadTicks < 1) {
        // A half-spread of zero would put the bid and ask at the same price, which
        // the engine would immediately self-match.
        throw std::runtime_error("market_maker: halfSpreadTicks must be >= 1");
    }
    if (p.requoteThresholdTicks < 1) {
        // Zero would requote on every tick regardless of whether anything moved,
        // producing pure churn and an unusable cancel ratio.
        throw std::runtime_error("market_maker: requoteThresholdTicks must be >= 1");
    }
    if (p.requoteIntervalMs < 0) {
        throw std::runtime_error("market_maker: requoteIntervalMs must be >= 0");
    }
    if (p.inventoryLimit < 1) {
        throw std::runtime_error("market_maker: inventoryLimit must be >= 1");
    }
    if (p.inventorySkewTicksPerUnit < 0.0) {
        // Negative would skew towards the side already accumulating, amplifying the
        // position instead of shedding it.
        throw std::runtime_error(
            "market_maker: inventorySkewTicksPerUnit must be >= 0");
    }
    if (p.maxInventorySkewTicks < 0) {
        throw std::runtime_error("market_maker: maxInventorySkewTicks must be >= 0");
    }
    if (p.inventoryUnitShares < 0) {
        throw std::runtime_error("market_maker: inventoryUnitShares must be >= 0");
    }
    if (p.referenceMode != "mid" && p.referenceMode != "last") {
        throw std::runtime_error(
            "market_maker: referenceMode must be \"mid\" or \"last\"");
    }
    return p;
}

long long MarketMaker::inventorySkewTicks(const MarketMakerParams& params, int inventory) {
    if (params.inventorySkewTicksPerUnit <= 0.0) return 0;
    const int unit = params.inventoryUnitShares > 0 ? params.inventoryUnitShares
                                                    : params.quoteSize;
    if (unit <= 0) return 0;

    const double units = static_cast<double>(inventory - params.inventoryTarget) / unit;
    long long ticks = std::llround(units * params.inventorySkewTicksPerUnit);
    const long long cap = params.maxInventorySkewTicks;
    if (ticks >  cap) ticks =  cap;
    if (ticks < -cap) ticks = -cap;
    return ticks;
}

void MarketMaker::onStart(StrategyContext& ctx) {
    params_ = MarketMakerParams::fromJson(ctx.params);
    started_ = true;
    ctx.log.note("market_maker: configured",
                 {{"quote_size", params_.quoteSize},
                  {"half_spread_ticks", params_.halfSpreadTicks},
                  {"requote_threshold_ticks", params_.requoteThresholdTicks},
                  {"inventory_limit", params_.inventoryLimit},
                  {"inventory_skew_ticks_per_unit", params_.inventorySkewTicksPerUnit},
                  {"reference_mode", params_.referenceMode}});
}

double MarketMaker::desiredPrice(const StrategyContext& ctx, bool buy,
                                 double reference, long long skew) const {
    const int inventory = ctx.position.inventory;

    // Hard inventory limit: withdraw the side that would make the position worse.
    // The skew alone only discourages that side; at the limit the maker must stop
    // quoting it outright, or persistent one-way flow walks the position past any
    // bound the skew can defend.
    if (buy && inventory >= params_.inventoryTarget + params_.inventoryLimit) return 0.0;
    if (!buy && inventory <= params_.inventoryTarget - params_.inventoryLimit) return 0.0;

    const long long offset = buy ? -params_.halfSpreadTicks : params_.halfSpreadTicks;
    double price = tickmath::shift(reference, offset - skew, ctx.tickSize);

    // Stay passive. A quote that crosses the far touch is a taker, not a maker, and
    // would turn every requote into an aggressive trade.
    if (ctx.market.isValid()) {
        if (buy && price >= ctx.market.bestAsk) {
            price = tickmath::shift(ctx.market.bestAsk, -1, ctx.tickSize);
        } else if (!buy && price <= ctx.market.bestBid) {
            price = tickmath::shift(ctx.market.bestBid, 1, ctx.tickSize);
        }
    }
    return price > 0.0 ? price : 0.0;
}

void MarketMaker::maintainSide(StrategyContext& ctx, bool buy, double desired) {
    Quote& quote = buy ? bid_ : ask_;

    // A quote stops existing when it fills or is cancelled; detecting that here also
    // covers fills we have not yet drained.
    if (quote.orderId != 0 && !ctx.orders.isLiveOrder(quote.orderId)) {
        quote.orderId = 0;
        quote.price = 0.0;
    }

    if (desired <= 0.0) {
        // Side withdrawn (inventory limit): pull any quote still resting there.
        if (quote.orderId != 0) {
            ctx.engine.cancel(quote.orderId);
            ++quoteCancels_;
            quote.orderId = 0;
            quote.price = 0.0;
        }
        return;
    }

    if (quote.orderId != 0) {
        const long long drift = std::llabs(tickmath::toIndex(desired, ctx.tickSize) -
                                           tickmath::toIndex(quote.price, ctx.tickSize));
        // Hysteresis: requoting on every sub-threshold wobble surrenders queue
        // position for nothing and buries the event log in churn.
        if (drift < params_.requoteThresholdTicks) return;
        ctx.engine.cancel(quote.orderId);
        ++quoteCancels_;
        quote.orderId = 0;
    }

    const unsigned int id = ctx.engine.submit(buy, desired, params_.quoteSize);
    if (id != 0) {
        quote.orderId = id;
        quote.price = desired;
        ++quoteUpdates_;
    }
}

void MarketMaker::onTick(StrategyContext& ctx) {
    if (!started_) return;

    double reference = 0.0;
    if (params_.referenceMode == "last" && ctx.market.lastPrice > 0.0) {
        reference = ctx.market.lastPrice;
    } else if (ctx.market.isValid()) {
        reference = ctx.market.midPrice();
    } else if (ctx.market.lastPrice > 0.0) {
        reference = ctx.market.lastPrice;   // fall back rather than stop quoting
    }
    if (!(reference > 0.0)) return;

    const int inventory = ctx.position.inventory;
    minInventory_ = std::min(minInventory_, inventory);
    maxInventory_ = std::max(maxInventory_, inventory);

    const long long skew = inventorySkewTicks(params_, inventory);
    maxAbsSkewTicks_ = std::max(maxAbsSkewTicks_, std::llabs(skew));

    const double desiredBid = desiredPrice(ctx, true, reference, skew);
    const double desiredAsk = desiredPrice(ctx, false, reference, skew);

    ++ticksObserved_;
    if (desiredBid <= 0.0) ++bidWithheldTicks_;
    if (desiredAsk <= 0.0) ++askWithheldTicks_;

    const auto sinceRequote = std::chrono::duration_cast<std::chrono::milliseconds>(
        ctx.now - lastRequoteAt_).count();
    if (sinceRequote >= params_.requoteIntervalMs) {
        maintainSide(ctx, true, desiredBid);
        maintainSide(ctx, false, desiredAsk);
        lastRequoteAt_ = ctx.now;
    }

    if (bid_.orderId != 0) ++ticksBidQuoted_;
    if (ask_.orderId != 0) ++ticksAskQuoted_;
    if (bid_.orderId != 0 && ask_.orderId != 0) {
        quotedSpreadSum_ += (ask_.price - bid_.price) / ctx.tickSize;
        ++quotedSpreadCount_;
    }
}

void MarketMaker::onFill(StrategyContext& ctx, const Fill& fill) {
    if (fill.buy) {
        ++bidFills_;
        bidSharesFilled_ += fill.size;
    } else {
        ++askFills_;
        askSharesFilled_ += fill.size;
    }
}

nlohmann::json MarketMaker::metrics() const {
    const double ticks = ticksObserved_ > 0 ? static_cast<double>(ticksObserved_) : 1.0;
    // Matched shares are the round-tripped portion; the rest is open inventory, whose
    // value depends on where the market ends up rather than on spread capture.
    const long long matched = std::min(bidSharesFilled_, askSharesFilled_);
    return {
        {"quote_updates", quoteUpdates_},
        {"quote_cancels", quoteCancels_},
        {"bid_fills", bidFills_},
        {"ask_fills", askFills_},
        {"bid_shares_filled", bidSharesFilled_},
        {"ask_shares_filled", askSharesFilled_},
        {"matched_shares", matched},
        {"fill_imbalance", (bidSharesFilled_ + askSharesFilled_) > 0
                               ? static_cast<double>(bidSharesFilled_ - askSharesFilled_) /
                                     (bidSharesFilled_ + askSharesFilled_)
                               : 0.0},
        {"min_inventory", minInventory_},
        {"max_inventory", maxInventory_},
        {"inventory_range", maxInventory_ - minInventory_},
        {"max_abs_skew_ticks", maxAbsSkewTicks_},
        {"bid_quoted_fraction", ticksBidQuoted_ / ticks},
        {"ask_quoted_fraction", ticksAskQuoted_ / ticks},
        {"bid_withheld_fraction", bidWithheldTicks_ / ticks},
        {"ask_withheld_fraction", askWithheldTicks_ / ticks},
        {"mean_quoted_spread_ticks",
             quotedSpreadCount_ > 0 ? quotedSpreadSum_ / quotedSpreadCount_ : 0.0},
    };
}
