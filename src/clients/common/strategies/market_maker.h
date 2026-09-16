#pragma once

#include "strategy.h"

#include <chrono>
#include <string>

// A basic inventory-aware market maker: quote both sides continuously, requote as the
// reference moves, and manage the position that accumulates from being filled.
//
// Deliberately NOT a second liquidity provider. The provider maintains a wide,
// multi-level ladder and is measured on depth; a market maker holds a thin quote near
// the touch and is measured on what its quoting costs and earns. The two differ in
// every dimension that matters:
//
//                     liquidity provider          market maker
//   shape             10-20 levels per side       one level per side
//   position          near the touch outward      at or inside the touch
//   requoting         on fills / recentring       continuously, on reference drift
//   inventory         shifts the whole ladder     shifts the quote AND stops one side
//   measured on       displayed depth             inventory, P&L, adverse selection
//
// The inventory response is the same idea proven in the provider (Ho-Stoll /
// Avellaneda-Stoikov): as the maker goes long it shifts its quotes DOWN, making its
// bid less attractive and its ask more attractive, until flow reverses. On top of
// that a hard inventory limit withdraws the side that would make the position worse,
// which is what keeps a maker solvent when flow is persistently one-directional.
//
// Deliberately simpler than a full Avellaneda-Stoikov implementation: no volatility
// estimate, no terminal-time horizon, no optimal-spread solution. Whether any of that
// earns its place is an experimental question, not an assumption.
struct MarketMakerParams {
    int    quoteSize             = 200;
    int    halfSpreadTicks       = 2;    // quote this far either side of the reference
    int    requoteThresholdTicks = 1;    // requote once the desired quote drifts this far
    int    requoteIntervalMs     = 100;  // floor on requote frequency

    int    inventoryTarget       = 0;
    int    inventoryLimit        = 2000; // stop quoting the side that worsens inventory
    int    inventoryUnitShares   = 0;    // 0 => quoteSize
    double inventorySkewTicksPerUnit = 1.0;
    int    maxInventorySkewTicks = 20;

    std::string referenceMode    = "mid";  // "mid" | "last"

    static MarketMakerParams fromJson(const nlohmann::json& j);
};

class MarketMaker : public Strategy {
public:
    // Ticks to shift the quotes for a given inventory. Positive means "shift down".
    // Pure and static so the inventory response is unit-testable on its own.
    static long long inventorySkewTicks(const MarketMakerParams& params, int inventory);

    void onStart(StrategyContext& ctx) override;
    void onTick(StrategyContext& ctx) override;
    void onFill(StrategyContext& ctx, const Fill& fill) override;
    nlohmann::json metrics() const override;

private:
    struct Quote {
        unsigned int orderId = 0;
        double       price   = 0.0;
    };

    // Returns 0 when the side should not be quoted at all.
    double desiredPrice(const StrategyContext& ctx, bool buy,
                        double reference, long long skew) const;
    void maintainSide(StrategyContext& ctx, bool buy, double desired);

    MarketMakerParams params_;
    Quote bid_;
    Quote ask_;
    bool  started_ = false;
    std::chrono::steady_clock::time_point lastRequoteAt_{};

    // metrics
    long long quoteUpdates_     = 0;
    long long quoteCancels_     = 0;
    long long bidFills_         = 0;
    long long askFills_         = 0;
    long long bidSharesFilled_  = 0;
    long long askSharesFilled_  = 0;
    long long ticksBidQuoted_   = 0;
    long long ticksAskQuoted_   = 0;
    long long ticksObserved_    = 0;
    long long bidWithheldTicks_ = 0;
    long long askWithheldTicks_ = 0;
    long long maxAbsSkewTicks_  = 0;
    int       minInventory_     = 0;
    int       maxInventory_     = 0;
    double    quotedSpreadSum_  = 0.0;
    long long quotedSpreadCount_ = 0;
};
