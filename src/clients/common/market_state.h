#pragma once

#include <vector>
#include <chrono>

struct PriceLevel {
    double price;
    int size;
    int rank;
};

// Local view of market state, updated by the watcher thread.
// Note: WatchSymbol returns front-of-queue size only, not aggregate depth.
struct MarketState {
    double bestBid     = 0.0;
    int    bestBidSize = 0;
    double bestAsk     = 0.0;
    int    bestAskSize = 0;
    double lastPrice   = 0.0;

    std::vector<PriceLevel> bidLevels; // from last GetLevel2 pull, best-to-worst
    std::vector<PriceLevel> askLevels;

    // Whole-market traded volume since this participant connected, from the public
    // tape rather than from our own fills. A participation-rate execution needs to
    // size itself against what the market is doing, and "our own fills" is exactly
    // the wrong denominator for that.
    long long observedVolume = 0;
    long long observedTrades = 0;

    std::chrono::steady_clock::time_point updatedAt;

    bool isValid() const {
        return bestBid > 0.0 && bestAsk > 0.0 && bestBid < bestAsk;
    }

    double midPrice() const { return (bestBid + bestAsk) / 2.0; }
    double spread()   const { return bestAsk - bestBid; }
};
