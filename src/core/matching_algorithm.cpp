//
//  matching_algorithm.cpp
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 3/8/24.
//

#include <stdio.h>
#include <chrono>
#include <iostream>
#include <deque>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#include "matching_algorithm.h"

using namespace std;

namespace {
int64_t nowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
}  // namespace

MatchingAlgorithm::MatchingAlgorithm(OrderBook& orderbook_in, std::shared_mutex& askMutex_in, std::shared_mutex& bidMutex_in) : orderbook(orderbook_in), askMutex(askMutex_in), bidMutex(bidMutex_in) {}

void MatchingAlgorithm::run(std::atomic<bool>& running, bool testFlag, bool timerShutoffMode) {
    int consecutiveIterationsWithoutMatch = 0;
    int consecutiveIterationsWithNoAskOrBid = 0;
    std::thread::id this_id = std::this_thread::get_id();
    const bool verbose = OrderBook::verbose.load();
    if (verbose) std::cout << "Thread ID " << this_id << ": Running " << orderbook.security << " matching algorithm..." << std::endl;

    bool timerStarted = false;
    auto startTime = std::chrono::steady_clock::time_point();

    // Reused across iterations to avoid reallocating every 1ms tick.
    std::vector<OrderBook::FillRecord> pendingFills;
    std::vector<unsigned int> filledBuyIds;
    std::vector<unsigned int> filledSellIds;

    // Publishing fills and reaping filled order ids happens AFTER the bid/ask tree
    // locks are released, so the matching critical section stays short and we never
    // take an order-hash mutex while holding a tree mutex.
    auto flushAfterUnlock = [&]() {
        if (!filledBuyIds.empty()) {
            std::unique_lock<std::shared_mutex> lock(orderbook.buyOrdersMutex);
            for (unsigned int id : filledBuyIds) orderbook.allBuyOrders.erase(id);
            filledBuyIds.clear();
        }
        if (!filledSellIds.empty()) {
            std::unique_lock<std::shared_mutex> lock(orderbook.sellOrdersMutex);
            for (unsigned int id : filledSellIds) orderbook.allSellOrders.erase(id);
            filledSellIds.clear();
        }
        for (const auto& rec : pendingFills) orderbook.publishFill(rec);
        pendingFills.clear();
    };

    while (running) {
        std::unique_lock<std::shared_mutex> lockBids(bidMutex, std::defer_lock);
        std::unique_lock<std::shared_mutex> lockAsks(askMutex, std::defer_lock);

        std::lock(lockBids, lockAsks);  // Lock both mutexes simultaneously
        if (!orderbook.askEmpty() && !orderbook.bidEmpty()) {
            if (timerShutoffMode && !timerStarted) {
                startTime = std::chrono::steady_clock::now();
                timerStarted = true;
                if (verbose) std::cout << "Matching timer started..." << std::endl;
            }
            consecutiveIterationsWithNoAskOrBid = 0;
            if (orderbook.getAsk() <= orderbook.getBid()) {
                consecutiveIterationsWithoutMatch = 0;
                
                LimitPrice* bestAsk = &(orderbook.askTree.begin()->second);
                LimitPrice* bestBid = &(orderbook.bidTree.begin()->second);

                // Retrieve best ask and bid orders
                const double ask = orderbook.askTree.begin()->first;
                const double bid = orderbook.bidTree.begin()->first;
                
                // Retrieve orders from best ask and bid
                std::deque<std::shared_ptr<OrderInternal>>& askOrders = bestAsk->orders;
                std::deque<std::shared_ptr<OrderInternal>>& bidOrders = bestBid->orders;
                
                // Iterate through the orders to match
                while (!bestAsk->orders.empty() && !bestBid->orders.empty() && bestAsk->orders.front()->price <= bestBid->orders.front()->price) {
                    std::shared_ptr<OrderInternal> askOrder = bestAsk->orders.front();
                    std::shared_ptr<OrderInternal> bidOrder = bestBid->orders.front();

                    // Price-time priority: whichever order arrived first is resting,
                    // so it is the maker and its limit sets the trade price.  The
                    // other side crossed the spread and is the aggressor.
                    const bool askIsMaker = askOrder->seq < bidOrder->seq;
                    const double fillPrice = askIsMaker ? askOrder->price : bidOrder->price;
                    const bool aggressorIsBuy = askIsMaker;
                    const unsigned int makerId = askIsMaker ? askOrder->orderId : bidOrder->orderId;
                    const unsigned int takerId = askIsMaker ? bidOrder->orderId : askOrder->orderId;

                    uint32_t filledSize = 0;
                    bool isBuy = false;
                    bool isPartial = false;
                    uint32_t remaining = 0;

                    if (askOrder->size > bidOrder->size) {
                        if (verbose) {
                            std::cout << "          Matched order: " << bidOrder->orderId << ": BID " << bidOrder->size << " shares at $" << fillPrice << std::endl;
                            std::cout << "Partially matched order: " << askOrder->orderId << ": ASK " << bidOrder->size << " shares at $" << fillPrice << std::endl;
                        }
                        filledSize = bidOrder->size;
                        askOrder->size -= filledSize;
                        remaining = askOrder->size;
                        orderbook.volume += filledSize;
                        bidOrders.pop_front();
                        bestBid->numOrders -= 1;
                        filledBuyIds.push_back(bidOrder->orderId);
                        isBuy = true; isPartial = true;

                    } else if (askOrder->size < bidOrder->size) {
                        if (verbose) {
                            std::cout << "          Matched order: " << askOrder->orderId << ": ASK " << askOrder->size << " shares at $" << fillPrice << std::endl;
                            std::cout << "Partially matched order: " << bidOrder->orderId << ": BID " << askOrder->size << " shares at $" << fillPrice << std::endl;
                        }
                        filledSize = askOrder->size;
                        bidOrder->size -= filledSize;
                        remaining = bidOrder->size;
                        orderbook.volume += filledSize;
                        askOrders.pop_front();
                        bestAsk->numOrders -= 1;
                        filledSellIds.push_back(askOrder->orderId);
                        isBuy = false; isPartial = true;
                    } else {
                        if (verbose) {
                            std::cout << "Matched order: " << askOrder->orderId << ": ASK " << askOrder->size << " shares at $" << fillPrice << std::endl;
                            std::cout << "Matched order: " << bidOrder->orderId << ": BID " << bidOrder->size << " shares at $" << fillPrice << std::endl;
                        }
                        filledSize = bidOrder->size;
                        askOrders.pop_front();
                        bidOrders.pop_front();
                        bestAsk->numOrders -= 1;
                        bestBid->numOrders -= 1;
                        filledSellIds.push_back(askOrder->orderId);
                        filledBuyIds.push_back(bidOrder->orderId);
                        orderbook.volume += filledSize;
                        isBuy = false; isPartial = false; remaining = 0;
                    }

                    // Displayed depth must shrink by the executed quantity on BOTH
                    // sides, in every one of the three cases above.  Omitting this was
                    // what made GetLevel2 sizes drift away from the resting orders.
                    bestAsk->totalSize -= filledSize;
                    bestBid->totalSize -= filledSize;

                    OrderBook::FillRecord rec;
                    rec.ts_ns            = nowNanos();
                    rec.price            = fillPrice;
                    rec.size             = filledSize;
                    rec.buy              = isBuy;
                    rec.partial_fill     = isPartial;
                    rec.remaining_size   = remaining;
                    rec.maker_order_id   = makerId;
                    rec.taker_order_id   = takerId;
                    rec.aggressor_is_buy = aggressorIsBuy;
                    pendingFills.push_back(rec);
                }
                // At this point, one or both LimitPrices have been exhausted, so they should be deleted from the book
                if (askOrders.empty()) {
                    if (verbose) std::cout << "Thread ID" << std::this_thread::get_id() << " Old Ask: " << ask << std::endl;
                    orderbook.askTree.erase(ask);
                    double newAsk = orderbook.refreshAskPointer(); // refreshAsk
                    if (verbose) std::cout << "Thread ID" << std::this_thread::get_id() << " New Ask: " << (newAsk != 0.0 ? std::to_string(newAsk) : "nullptr") << std::endl;
                }
                if (bidOrders.empty()) {
                    if (verbose) std::cout << "Thread ID" << std::this_thread::get_id() << " Old Bid: " << bid << std::endl;
                    orderbook.bidTree.erase(bid);
                    double newBid = orderbook.refreshBidPointer();
                    if (verbose) std::cout << "Thread ID" << std::this_thread::get_id() << " New Bid: " << (newBid != 0.0 ? std::to_string(newBid) : "nullptr") << std::endl;
                }
                if (timerShutoffMode && orderbook.ask == nullptr && orderbook.bid == nullptr && timerStarted) {
                    auto endTime = std::chrono::steady_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
                    lockBids.unlock();
                    lockAsks.unlock();
                    flushAfterUnlock();
                    if (verbose) std::cout << "Matching completed in " << duration << " milliseconds." << std::endl;
                    return; // Stop the algorithm since we are done
                }
            } else if (testFlag && ++consecutiveIterationsWithoutMatch >= 20) {
                lockBids.unlock();
                lockAsks.unlock();
                flushAfterUnlock();
                return;
            }
        }
        if (testFlag && ++consecutiveIterationsWithNoAskOrBid >= 20) {
            lockBids.unlock();
            lockAsks.unlock();
            flushAfterUnlock();
            return;
        }

        // Sleep for a certain period before checking again (to prevent busy waiting)
        lockBids.unlock();
        lockAsks.unlock();
        flushAfterUnlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    flushAfterUnlock();
    if (verbose) std::cout << "Thread ID " << this_id << ": " << orderbook.security << " Matching algorithm halted..." << std::endl;
}
