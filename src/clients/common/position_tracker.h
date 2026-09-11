#pragma once

#include <string>

// Tracks a participant's inventory and P&L from its fills.
//
// Uses average-cost accounting: buys and sells that increase the absolute
// position update the average entry price; trades that reduce the position
// realize P&L against that average.  A sign flip (e.g. long -> short) realizes
// P&L on the closed portion and re-bases the average on the opened portion.
//
// Fill attribution upstream (OrderTracker) is heuristic, so inventory is exact
// only when this participant is the sole order owner at a filled price level.
struct PositionTracker {
    std::string symbol;
    int    inventory        = 0;    // +long, -short (shares)
    double realizedPnl      = 0.0;
    double averageEntryPrice = 0.0; // average cost of the current open position
    long long buySharesFilled  = 0;
    long long sellSharesFilled = 0;

    void onFill(bool buy, int size, double price);
    double unrealizedPnl(double currentPrice) const;
};
