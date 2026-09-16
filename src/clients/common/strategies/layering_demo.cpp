#include "layering_demo.h"

#include "tick_math.h"

#include <stdexcept>

LayeringParams LayeringParams::fromJson(const nlohmann::json& j) {
    LayeringParams p;
    p.side              = j.value("side", p.side);
    p.layers            = j.value("layers", p.layers);
    p.layerSize         = j.value("layerSize", p.layerSize);
    p.startOffsetTicks  = j.value("startOffsetTicks", p.startOffsetTicks);
    p.layerSpacingTicks = j.value("layerSpacingTicks", p.layerSpacingTicks);
    p.holdMs            = j.value("holdMs", p.holdMs);
    p.cycleIntervalMs   = j.value("cycleIntervalMs", p.cycleIntervalMs);
    p.cycles            = j.value("cycles", p.cycles);
    p.tradeOpposite     = j.value("tradeOpposite", p.tradeOpposite);
    p.opposingSize      = j.value("opposingSize", p.opposingSize);

    if (p.side != "BUY" && p.side != "SELL") {
        throw std::runtime_error("layering_demo: side must be \"BUY\" or \"SELL\"");
    }
    if (p.layers < 1) {
        throw std::runtime_error("layering_demo: layers must be >= 1");
    }
    if (p.layerSize < 1) {
        throw std::runtime_error("layering_demo: layerSize must be >= 1");
    }
    if (p.startOffsetTicks < 1) {
        // Zero would put a layer at the touch, where it could actually trade. The
        // pattern depends on the orders being displayed but not executed.
        throw std::runtime_error("layering_demo: startOffsetTicks must be >= 1");
    }
    if (p.layerSpacingTicks < 1) {
        throw std::runtime_error("layering_demo: layerSpacingTicks must be >= 1");
    }
    if (p.holdMs < 0 || p.cycleIntervalMs < 0) {
        throw std::runtime_error("layering_demo: durations must be >= 0");
    }
    if (p.cycles < 0) {
        throw std::runtime_error("layering_demo: cycles must be >= 0");
    }
    if (p.tradeOpposite && p.opposingSize < 1) {
        throw std::runtime_error("layering_demo: opposingSize must be >= 1");
    }
    return p;
}

void LayeringDemo::onStart(StrategyContext& ctx) {
    params_ = LayeringParams::fromJson(ctx.params);
    phaseStartedAt_ = ctx.now;
    ctx.log.note("layering_demo: configured",
                 {{"side", params_.side},
                  {"layers", params_.layers},
                  {"layer_size", params_.layerSize},
                  {"hold_ms", params_.holdMs},
                  {"cycle_interval_ms", params_.cycleIntervalMs},
                  {"trade_opposite", params_.tradeOpposite}});
}

void LayeringDemo::placeLayers(StrategyContext& ctx) {
    const double touch = params_.isBuy() ? ctx.market.bestBid : ctx.market.bestAsk;
    if (!(touch > 0.0)) return;

    for (int i = 0; i < params_.layers; ++i) {
        const long long offset =
            params_.startOffsetTicks + static_cast<long long>(i) * params_.layerSpacingTicks;
        // Behind the touch, never at or through it: these orders are meant to be seen,
        // not filled.
        const double price = tickmath::shift(
            touch, params_.isBuy() ? -offset : offset, ctx.tickSize);
        if (!(price > 0.0)) continue;

        const unsigned int id = ctx.engine.submit(params_.isBuy(), price, params_.layerSize);
        if (id != 0) {
            layerIds_.push_back(id);
            ++layersPlaced_;
            displayedQuantity_ += params_.layerSize;
        }
    }

    if (params_.tradeOpposite) {
        // The small genuine order, on the other side, while the book looks lopsided.
        const double farTouch = params_.isBuy() ? ctx.market.bestBid : ctx.market.bestAsk;
        if (farTouch > 0.0) {
            const unsigned int id =
                ctx.engine.submit(!params_.isBuy(), farTouch, params_.opposingSize);
            if (id != 0) ++opposingTrades_;
        }
    }

    ctx.log.note("layering_demo: layers displayed",
                 {{"orders", params_.layers},
                  {"displayed_quantity", params_.layers * params_.layerSize}});
}

void LayeringDemo::pullLayers(StrategyContext& ctx) {
    for (unsigned int id : layerIds_) {
        if (ctx.orders.isLiveOrder(id)) {
            ctx.engine.cancel(id);
            ++layersCancelled_;
        }
    }
    layerIds_.clear();
    ctx.log.note("layering_demo: layers withdrawn", {{"cycle", cyclesCompleted_ + 1}});
}

void LayeringDemo::onTick(StrategyContext& ctx) {
    if (complete_) return;
    if (!ctx.market.isValid()) return;

    const auto inPhase = std::chrono::duration_cast<std::chrono::milliseconds>(
        ctx.now - phaseStartedAt_).count();

    if (phase_ == Phase::Waiting) {
        if (cyclesCompleted_ == 0 || inPhase >= params_.cycleIntervalMs) {
            placeLayers(ctx);
            phase_ = Phase::Displayed;
            phaseStartedAt_ = ctx.now;
        }
        return;
    }

    if (inPhase >= params_.holdMs) {
        pullLayers(ctx);
        ++cyclesCompleted_;
        phase_ = Phase::Waiting;
        phaseStartedAt_ = ctx.now;
        if (params_.cycles > 0 && cyclesCompleted_ >= params_.cycles) {
            complete_ = true;
        }
    }
}

void LayeringDemo::onFill(StrategyContext& ctx, const Fill& fill) {
    // A layer that trades is the pattern failing: the whole point is to withdraw
    // before anyone can hit it. Counting these separately makes that visible rather
    // than hiding it in a total.
    bool wasLayer = false;
    for (unsigned int id : layerIds_) {
        if (id == fill.orderId) { wasLayer = true; break; }
    }
    if (wasLayer) layersFilled_ += fill.size;
    else          opposingFilled_ += fill.size;
}

void LayeringDemo::onStop(StrategyContext& ctx) {
    pullLayers(ctx);
}

nlohmann::json LayeringDemo::metrics() const {
    return {
        {"side", params_.side},
        {"cycles_completed", cyclesCompleted_},
        {"layers_placed", layersPlaced_},
        {"layers_cancelled", layersCancelled_},
        {"layers_filled_shares", layersFilled_},
        {"displayed_quantity_total", displayedQuantity_},
        {"opposing_orders", opposingTrades_},
        {"opposing_filled_shares", opposingFilled_},
        // The headline surveillance signal: essentially everything posted is
        // withdrawn without trading.
        {"layer_cancel_ratio",
             layersPlaced_ > 0 ? static_cast<double>(layersCancelled_) / layersPlaced_ : 0.0},
    };
}
