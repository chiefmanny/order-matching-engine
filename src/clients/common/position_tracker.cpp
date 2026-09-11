#include "position_tracker.h"

#include <cmath>
#include <cstdlib>

void PositionTracker::onFill(bool buy, int size, double price) {
    if (size <= 0) return;

    const int signedQty = buy ? size : -size;

    if (buy) buySharesFilled  += size;
    else     sellSharesFilled += size;

    const int oldInventory = inventory;
    const int newInventory = oldInventory + signedQty;

    const bool increasing =
        (oldInventory == 0) ||
        (oldInventory > 0 && signedQty > 0) ||
        (oldInventory < 0 && signedQty < 0);

    if (increasing) {
        // Adding to (or opening) the position: update weighted-average cost.
        const double oldAbs = std::abs(static_cast<double>(oldInventory));
        const double addAbs = static_cast<double>(size);
        const double totalAbs = oldAbs + addAbs;
        if (totalAbs > 0.0) {
            averageEntryPrice =
                (averageEntryPrice * oldAbs + price * addAbs) / totalAbs;
        }
        inventory = newInventory;
        return;
    }

    // Reducing the position: realize P&L on the closed shares against average.
    const int closedShares = std::min(std::abs(signedQty), std::abs(oldInventory));
    if (oldInventory > 0) {
        // Closing longs by selling: profit = (sell - avg) * shares.
        realizedPnl += (price - averageEntryPrice) * closedShares;
    } else {
        // Closing shorts by buying: profit = (avg - buy) * shares.
        realizedPnl += (averageEntryPrice - price) * closedShares;
    }

    if (std::abs(signedQty) <= std::abs(oldInventory)) {
        // Position reduced but not flipped; average price unchanged.
        inventory = newInventory;
        if (inventory == 0) averageEntryPrice = 0.0;
    } else {
        // Position flipped through zero: the opened portion re-bases the average.
        inventory = newInventory;
        averageEntryPrice = price;
    }
}

double PositionTracker::unrealizedPnl(double currentPrice) const {
    if (inventory == 0) return 0.0;
    return (currentPrice - averageEntryPrice) * inventory;
}
