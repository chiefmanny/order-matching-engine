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
    p.withdrawAfterSeconds     = j.value("withdrawAfterSeconds", p.withdrawAfterSeconds);
    p.withdrawDurationMs       = j.value("withdrawDurationMs", p.withdrawDurationMs);
    p.withdrawOrder            = j.value("withdrawOrder", p.withdrawOrder);

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
    // smaller averageQuantity, which configuration already expresses.
    if (p.mode != "normal" && p.mode != "withdraw") {
        throw std::runtime_error("liquidity_provider: unsupported mode '" + p.mode +
                                 "' (expected 'normal' or 'withdraw')");
    }
    if (p.mode == "withdraw") {
        if (p.withdrawAfterSeconds < 0.0) {
            throw std::runtime_error(
                "liquidity_provider: withdrawAfterSeconds must be >= 0");
        }
        if (p.withdrawDurationMs < 0) {
            throw std::runtime_error(
                "liquidity_provider: withdrawDurationMs must be >= 0");
        }
        if (p.withdrawOrder != "inside_out" && p.withdrawOrder != "outside_in") {
            throw std::runtime_error(
                "liquidity_provider: withdrawOrder must be \"inside_out\" or \"outside_in\"");
        }
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

bool LiquidityProvider::handleWithdrawal(StrategyContext& ctx) {
    if (params_.mode != "withdraw") return false;
    if (ctx.elapsedSeconds < params_.withdrawAfterSeconds) return false;

    if (!withdrawing_) {
        withdrawing_ = true;
        // Anchor progress to the CONFIGURED moment, not to the first tick that
        // happened to observe it. Anchoring to the tick makes the opening tick always
        // cancel nothing, and makes the whole schedule drift with the decision
        // interval -- so the same scenario would withdraw at different rates
        // depending on an unrelated setting.
        withdrawBeganAt_ = params_.withdrawAfterSeconds;
        ctx.log.note("liquidity_provider: withdrawing liquidity",
                     {{"order", params_.withdrawOrder},
                      {"duration_ms", params_.withdrawDurationMs},
                      {"levels", slots_.size()}});
    }
    if (withdrawComplete_) return true;

    const double elapsedMs = (ctx.elapsedSeconds - withdrawBeganAt_) * 1000.0;
    double progress = params_.withdrawDurationMs > 0
                          ? elapsedMs / params_.withdrawDurationMs
                          : 1.0;
    if (progress < 0.0) progress = 0.0;
    if (progress > 1.0) progress = 1.0;

    // Sort each side by distance from the ladder centre, then pull a prefix whose
    // size grows with elapsed progress. Doing it by proportion rather than by a
    // per-tick count keeps the pace independent of the decision interval.
    std::vector<long long> bids, asks;
    for (const auto& entry : slots_) {
        (entry.second.level.buy ? bids : asks).push_back(entry.first);
    }
    // Bids ascend away from the centre as the index falls; asks as it rises.
    std::sort(bids.begin(), bids.end(), std::greater<long long>());  // inside first
    std::sort(asks.begin(), asks.end());                             // inside first
    if (params_.withdrawOrder == "outside_in") {
        std::reverse(bids.begin(), bids.end());
        std::reverse(asks.begin(), asks.end());
    }

    auto pullFrom = [&](const std::vector<long long>& ordered) {
        const size_t target =
            static_cast<size_t>(std::llround(progress * static_cast<double>(ordered.size())));
        for (size_t i = 0; i < target && i < ordered.size(); ++i) {
            auto it = slots_.find(ordered[i]);
            if (it == slots_.end()) continue;
            if (it->second.orderId != 0 && ctx.orders.isLiveOrder(it->second.orderId)) {
                ctx.engine.cancel(it->second.orderId);
                ++levelsWithdrawn_;
            }
            // Blocking replenishment matters as much as the cancel: without it the
            // reconcile loop would refill the level on the very next tick.
            it->second.orderId = 0;
            it->second.readyAt = ctx.now + std::chrono::hours(24);
        }
    };
    pullFrom(bids);
    pullFrom(asks);

    if (progress >= 1.0 && !withdrawComplete_) {
        withdrawComplete_ = true;
        ctx.log.note("liquidity_provider: withdrawal complete",
                     {{"levels_withdrawn", levelsWithdrawn_}});
    }
    return true;
}

void LiquidityProvider::onTick(StrategyContext& ctx) {
    if (!started_) return;

    // Withdrawal replaces the reconcile loop entirely: re-centring or replenishing
    // while pulling the book would fight the withdrawal.
    if (handleWithdrawal(ctx)) {
        ++ticksObserved_;
        lastTickAt_ = ctx.now;
        return;
    }

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
        {"mode", params_.mode},
        {"withdrawing", withdrawing_},
        {"withdraw_complete", withdrawComplete_},
        {"levels_withdrawn", levelsWithdrawn_},
    };
}
