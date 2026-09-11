#include "liquidity_provider.h"

#include "tick_math.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace {

using tickmath::fromIndex;
using tickmath::toIndex;

}  // namespace

LadderParams LadderParams::fromJson(const nlohmann::json& j) {
    LadderParams p;
    if (!j.contains("referencePrice")) {
        throw std::runtime_error("liquidity_provider requires parameters.referencePrice");
    }
    p.referencePrice           = j.at("referencePrice").get<double>();
    p.bidLevels                = j.value("bidLevels", p.bidLevels);
    p.askLevels                = j.value("askLevels", p.askLevels);
    p.halfSpreadTicks          = j.value("halfSpreadTicks", p.halfSpreadTicks);
    p.levelSpacingTicks        = j.value("levelSpacingTicks", p.levelSpacingTicks);
    p.averageQuantity          = j.value("averageQuantity", p.averageQuantity);
    p.quantityVariance         = j.value("quantityVariance", p.quantityVariance);
    p.levelSpacingJitterTicks  = j.value("levelSpacingJitterTicks", p.levelSpacingJitterTicks);
    p.maxDistanceFromReference = j.value("maxDistanceFromReference", p.maxDistanceFromReference);
    p.replenishDelayMs         = j.value("replenishDelayMs", p.replenishDelayMs);
    p.recenterOnMid            = j.value("recenterOnMid", p.recenterOnMid);
    p.recenterThresholdTicks   = j.value("recenterThresholdTicks", p.recenterThresholdTicks);
    p.inventoryTarget          = j.value("inventoryTarget", p.inventoryTarget);
    p.inventoryUnitShares      = j.value("inventoryUnitShares", p.inventoryUnitShares);
    p.inventorySkewTicksPerUnit = j.value("inventorySkewTicksPerUnit", p.inventorySkewTicksPerUnit);
    p.maxInventorySkewTicks    = j.value("maxInventorySkewTicks", p.maxInventorySkewTicks);
    p.mode                     = j.value("mode", p.mode);

    if (!(p.referencePrice > 0.0)) {
        throw std::runtime_error("liquidity_provider: referencePrice must be > 0");
    }
    if (p.bidLevels < 0 || p.askLevels < 0 || (p.bidLevels == 0 && p.askLevels == 0)) {
        throw std::runtime_error("liquidity_provider: needs at least one bid or ask level");
    }
    if (p.levelSpacingTicks < 1) {
        throw std::runtime_error("liquidity_provider: levelSpacingTicks must be >= 1");
    }
    if (p.halfSpreadTicks < 1) {
        // A half-spread of 0 would place a bid and an ask at the same price, which
        // the engine would immediately self-match.
        throw std::runtime_error("liquidity_provider: halfSpreadTicks must be >= 1");
    }
    if (p.averageQuantity < 1) {
        throw std::runtime_error("liquidity_provider: averageQuantity must be >= 1");
    }
    if (p.quantityVariance < 0 || p.quantityVariance >= p.averageQuantity) {
        throw std::runtime_error(
            "liquidity_provider: quantityVariance must be >= 0 and < averageQuantity");
    }
    if (p.inventorySkewTicksPerUnit < 0.0) {
        // A negative coefficient would shift the ladder TOWARDS the accumulating side,
        // amplifying inventory instead of shedding it -- a runaway, not a market.
        throw std::runtime_error(
            "liquidity_provider: inventorySkewTicksPerUnit must be >= 0");
    }
    if (p.inventoryUnitShares < 0) {
        throw std::runtime_error("liquidity_provider: inventoryUnitShares must be >= 0");
    }
    if (p.maxInventorySkewTicks < 0) {
        throw std::runtime_error("liquidity_provider: maxInventorySkewTicks must be >= 0");
    }
    if (p.recenterOnMid && p.recenterThresholdTicks < 1) {
        throw std::runtime_error(
            "liquidity_provider: recenterOnMid requires recenterThresholdTicks >= 1");
    }
    // `thin` is deliberately NOT a mode: a thin book is just fewer levels and a
    // smaller averageQuantity, which configuration already expresses.  `withdraw`
    // arrives in Phase 10 and must not silently behave like `normal` before then.
    if (p.mode != "normal") {
        throw std::runtime_error("liquidity_provider: unsupported mode '" + p.mode +
                                 "' (only 'normal' is implemented; 'withdraw' lands in Phase 10)");
    }
    return p;
}

std::vector<LiquidityProvider::Level> LiquidityProvider::buildLadder(
    const LadderParams& params, double referencePrice, double tickSize, RandomSource& rng) {
    std::vector<Level> ladder;

    const long long refTick = toIndex(referencePrice, tickSize);
    const long long maxTicksOut =
        params.maxDistanceFromReference > 0.0
            ? static_cast<long long>(std::floor(params.maxDistanceFromReference / tickSize))
            : 0;

    auto sizeForLevel = [&]() {
        return params.quantityVariance > 0
                   ? rng.sizeAround(params.averageQuantity, params.quantityVariance)
                   : params.averageQuantity;
    };

    auto jitter = [&]() {
        return params.levelSpacingJitterTicks > 0
                   ? rng.intInRange(-params.levelSpacingJitterTicks,
                                    params.levelSpacingJitterTicks)
                   : 0;
    };

    // Bids walk down from the near touch, asks walk up. Both sides are generated in
    // the same order every call so a fixed seed reproduces the same ladder.
    for (int i = 0; i < params.bidLevels; ++i) {
        long long offset = params.halfSpreadTicks +
                           static_cast<long long>(i) * params.levelSpacingTicks + jitter();
        if (offset < params.halfSpreadTicks) offset = params.halfSpreadTicks;
        if (maxTicksOut > 0 && offset > maxTicksOut) continue;

        const long long tickIndex = refTick - offset;
        const double price = fromIndex(tickIndex, tickSize);
        if (price <= 0.0) continue;  // never submit a non-positive price
        ladder.push_back(Level{tickIndex, price, sizeForLevel(), true});
    }

    for (int i = 0; i < params.askLevels; ++i) {
        long long offset = params.halfSpreadTicks +
                           static_cast<long long>(i) * params.levelSpacingTicks + jitter();
        if (offset < params.halfSpreadTicks) offset = params.halfSpreadTicks;
        if (maxTicksOut > 0 && offset > maxTicksOut) continue;

        const long long tickIndex = refTick + offset;
        ladder.push_back(Level{tickIndex, fromIndex(tickIndex, tickSize),
                               sizeForLevel(), false});
    }

    return ladder;
}

long long LiquidityProvider::inventorySkewTicks(const LadderParams& params, int inventory) {
    if (params.inventorySkewTicksPerUnit <= 0.0) return 0;  // rigid wall: no response

    const int unit = params.inventoryUnitShares > 0 ? params.inventoryUnitShares
                                                    : params.averageQuantity;
    if (unit <= 0) return 0;

    const double units = static_cast<double>(inventory - params.inventoryTarget) / unit;
    long long ticks = std::llround(units * params.inventorySkewTicksPerUnit);

    const long long cap = params.maxInventorySkewTicks;
    if (ticks >  cap) ticks =  cap;
    if (ticks < -cap) ticks = -cap;
    return ticks;
}

void LiquidityProvider::onStart(StrategyContext& ctx) {
    params_    = LadderParams::fromJson(ctx.params);
    reference_ = params_.referencePrice;
    lastTickAt_ = ctx.now;
    ladderCenterTick_ = toIndex(reference_, ctx.tickSize);
    minCenterTick_ = maxCenterTick_ = ladderCenterTick_;
    rebuildDesired(ctx, reference_);
    started_ = true;

    ctx.log.note("liquidity_provider: ladder configured",
                 {{"reference_price", reference_},
                  {"bid_levels", params_.bidLevels},
                  {"ask_levels", params_.askLevels},
                  {"half_spread_ticks", params_.halfSpreadTicks},
                  {"level_spacing_ticks", params_.levelSpacingTicks},
                  {"average_quantity", params_.averageQuantity},
                  {"inventory_skew_ticks_per_unit", params_.inventorySkewTicksPerUnit},
                  {"desired_levels", slots_.size()}});
}

void LiquidityProvider::rebuildDesired(StrategyContext& ctx, double centerPrice) {
    auto ladder = buildLadder(params_, centerPrice, ctx.tickSize, ctx.rng);
    ladderCenterTick_ = toIndex(centerPrice, ctx.tickSize);
    minCenterTick_ = std::min(minCenterTick_, ladderCenterTick_);
    maxCenterTick_ = std::max(maxCenterTick_, ladderCenterTick_);

    std::map<long long, Slot> next;
    for (const auto& level : ladder) {
        auto existing = slots_.find(level.tickIndex);
        if (existing != slots_.end()) {
            // Keep the live order and its replenish timer; only the target changes.
            Slot slot = existing->second;
            slot.level = level;
            next.emplace(level.tickIndex, slot);
            slots_.erase(existing);
        } else {
            next.emplace(level.tickIndex, Slot{level, 0, ctx.now});
        }
    }

    // Anything left in slots_ is no longer part of the ladder (re-centering moved it).
    for (auto& entry : slots_) {
        if (entry.second.orderId != 0 && ctx.orders.isLiveOrder(entry.second.orderId)) {
            ctx.engine.cancel(entry.second.orderId);
        }
    }
    slots_ = std::move(next);
}

void LiquidityProvider::drainFills(StrategyContext& ctx) {
    std::vector<std::pair<unsigned int, std::chrono::steady_clock::time_point>> fills;
    {
        std::lock_guard<std::mutex> lock(fillMutex_);
        fills.swap(pendingFills_);
    }
    for (const auto& entry : fills) {
        for (auto& slotEntry : slots_) {
            if (slotEntry.second.orderId == entry.first) {
                // Delay is measured from the execution instant, not from this tick.
                slotEntry.second.readyAt =
                    entry.second + std::chrono::milliseconds(params_.replenishDelayMs);
                break;
            }
        }
    }
}

void LiquidityProvider::onTick(StrategyContext& ctx) {
    if (!started_) return;

    // Re-center on a sustained mid move so the ladder follows the market instead of
    // being walked through and left behind.
    if (params_.recenterOnMid && ctx.market.isValid()) {
        const double mid = ctx.market.midPrice();
        const double drift = std::fabs(mid - reference_);
        if (drift > params_.recenterThresholdTicks * ctx.tickSize) {
            const double newReference =
                fromIndex(toIndex(mid, ctx.tickSize), ctx.tickSize);
            ctx.log.note("liquidity_provider: recentering",
                         {{"from", reference_}, {"to", newReference}});
            reference_ = newReference;
            ++recenters_;
        }
    }

    // Inventory response. Long inventory shifts the whole ladder DOWN, which makes
    // our bids less attractive and our asks more attractive until flow reverses.
    // This is what lets order-flow pressure move price instead of being absorbed.
    const int inventory = ctx.position.inventory;
    const long long skew = inventorySkewTicks(params_, inventory);
    maxAbsSkewTicks_ = std::max(maxAbsSkewTicks_, std::llabs(skew));
    maxAbsInventory_ = std::max(maxAbsInventory_, static_cast<long long>(std::abs(inventory)));

    const long long desiredCenterTick = toIndex(reference_, ctx.tickSize) - skew;
    if (desiredCenterTick != ladderCenterTick_) {
        const double newCenter = fromIndex(desiredCenterTick, ctx.tickSize);
        ctx.log.note("liquidity_provider: shifting on inventory",
                     {{"inventory", inventory},
                      {"skew_ticks", skew},
                      {"center", newCenter}});
        ++skewShifts_;
        rebuildDesired(ctx, newCenter);
    }

    drainFills(ctx);

    int liveLevels = 0;
    long long bidDepth = 0;
    long long askDepth = 0;

    for (auto& entry : slots_) {
        Slot& slot = entry.second;

        // A slot stops being live when its order fills or is canceled. Detecting it
        // here (rather than only in onFill) also covers cancels we did not initiate.
        if (slot.orderId != 0 && !ctx.orders.isLiveOrder(slot.orderId)) {
            slot.orderId = 0;
            if (slot.readyAt < ctx.now) {
                slot.readyAt = ctx.now + std::chrono::milliseconds(params_.replenishDelayMs);
            }
        }

        if (slot.orderId == 0) {
            if (ctx.now >= slot.readyAt) {
                const unsigned int id =
                    ctx.engine.submit(slot.level.buy, slot.level.price, slot.level.size);
                if (id != 0) {
                    slot.orderId = id;
                    ++replenishments_;
                }
            }
        }

        if (slot.orderId != 0) {
            ++liveLevels;
            if (slot.level.buy) bidDepth += slot.level.size;
            else                askDepth += slot.level.size;
        }
    }

    ++ticksObserved_;
    liveLevelSamples_ += liveLevels;
    bidDepthSamples_  += bidDepth;
    askDepthSamples_  += askDepth;
    if (liveLevels < static_cast<int>(slots_.size())) {
        secondsIncomplete_ += std::chrono::duration<double>(ctx.now - lastTickAt_).count();
    }
    lastTickAt_ = ctx.now;
}

void LiquidityProvider::onFill(StrategyContext& ctx, const Fill& fill) {
    std::lock_guard<std::mutex> lock(fillMutex_);
    pendingFills_.emplace_back(fill.orderId, std::chrono::steady_clock::now());
    ++fillsObserved_;
    sharesFilled_ += fill.size;
}

nlohmann::json LiquidityProvider::metrics() const {
    const double ticks = ticksObserved_ > 0 ? static_cast<double>(ticksObserved_) : 1.0;
    return {
        {"reference_price", reference_},
        {"desired_levels", slots_.size()},
        {"replenishments", replenishments_},
        {"recenters", recenters_},
        {"skew_shifts", skewShifts_},
        {"max_abs_skew_ticks", maxAbsSkewTicks_},
        {"max_abs_inventory", maxAbsInventory_},
        {"ladder_center_range_ticks", maxCenterTick_ - minCenterTick_},
        {"fills_observed", fillsObserved_},
        {"shares_filled", sharesFilled_},
        {"mean_live_levels", liveLevelSamples_ / ticks},
        {"mean_bid_depth", bidDepthSamples_ / ticks},
        {"mean_ask_depth", askDepthSamples_ / ticks},
        {"seconds_with_incomplete_ladder", secondsIncomplete_},
    };
}
