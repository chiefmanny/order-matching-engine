#include "echo_strategy.h"

#include "tick_math.h"

void EchoStrategy::onStart(StrategyContext& ctx) {
    price_       = ctx.params.value("price", 500.00);
    size_        = ctx.params.value("size", 100);
    crossAfterMs_ = ctx.params.value("crossAfterMs", 0);
    holdMs_      = ctx.params.value("holdMs", 2000);
    price_ = tickmath::round(price_, ctx.tickSize);

    // A single passive bid, well below any plausible ask so it rests rather than trades.
    restingId_ = ctx.engine.submit(true, price_, size_);
    if (restingId_ == 0) {
        ctx.log.note("echo: initial submit was rejected");
        complete_ = true;
        return;
    }
    ctx.log.note("echo: resting order placed",
                 {{"order_id", restingId_}, {"price", price_}, {"size", size_}});
}

void EchoStrategy::onTick(StrategyContext& ctx) {
    const double elapsedMs = ctx.elapsedSeconds * 1000.0;

    // Optionally trade with ourselves, to prove fill attribution by order id
    // end-to-end: the engine reports both maker and taker, and both are ours.
    if (crossAfterMs_ > 0 && !crossed_ && elapsedMs >= crossAfterMs_) {
        crossed_ = true;
        const unsigned int id = ctx.engine.submit(false, price_, size_);
        ctx.log.note("echo: submitted crossing sell", {{"order_id", id}});
        return;
    }

    if (elapsedMs >= holdMs_) {
        // Cancel whatever is still live and finish. The runtime would clean up
        // anyway; doing it here exercises the cancel path explicitly.
        if (ctx.orders.isLiveOrder(restingId_)) {
            ctx.engine.cancel(restingId_);
        }
        complete_ = true;
    }
}

void EchoStrategy::onFill(StrategyContext& ctx, const Fill& fill) {
    ++fillsObserved_;
    ctx.log.note("echo: observed own fill",
                 {{"order_id", fill.orderId},
                  {"size", fill.size},
                  {"price", fill.price},
                  {"is_maker", fill.isMaker}});
}

nlohmann::json EchoStrategy::metrics() const {
    return {{"resting_order_id", restingId_},
            {"fills_observed", fillsObserved_},
            {"crossed", crossed_}};
}
