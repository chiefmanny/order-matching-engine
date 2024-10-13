//
//  matching_algorithm.cpp
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 3/8/24.
//

#include <stdio.h>
#include <iostream>
#include <deque>
#include <memory>
#include <atomic>
#include <mutex>
#include "matching_algorithm.h"

using namespace std;

MatchingAlgorithm::MatchingAlgorithm(OrderBook& orderbook_in, std::shared_mutex& askMutex_in, std::shared_mutex& bidMutex_in) : orderbook(orderbook_in), askMutex(askMutex_in), bidMutex(bidMutex_in) {}

void MatchingAlgorithm::run(std::atomic<bool>& running, bool testFlag = false, bool timerShutoffMode) {
    int consecutiveIterationsWithoutMatch = 0;
    int consecutiveIterationsWithNoAskOrBid = 0;
    int iterationCount = 0;
    std::thread::id this_id = std::this_thread::get_id();
    std::cout << "Thread ID " << this_id << ": Running " << orderbook.security << " matching algorithm..." << std::endl;

    bool timerStarted = false;
    auto startTime = std::chrono::steady_clock::time_point();
    
    while (running) {
        {
            std::shared_lock<std::shared_mutex> lockAsksShared(askMutex);
            std::shared_lock<std::shared_mutex> lockBidsShared(bidMutex);

            if (!orderbook.askEmpty() && !orderbook.bidEmpty()) {
                consecutiveIterationsWithNoAskOrBid = 0;

                if (orderbook.getAsk() <= orderbook.getBid()) {
                    if (timerShutoffMode && !timerStarted) {
                        startTime = std::chrono::steady_clock::now();
                        timerStarted = true;
                        std::cout << "Matching timer started..." << std::endl;
                    }
                    consecutiveIterationsWithoutMatch = 0;
                    
                    auto askIt = orderbook.askTree.begin();
                    auto bidIt = orderbook.bidTree.begin();
                    LimitPrice* bestAsk = &(askIt->second);
                    LimitPrice* bestBid = &(bidIt->second);

                    // Retrieve best ask and bid orders
                    const double ask = askIt->first;
                    const double bid = bidIt->first;

                    std::unique_lock<std::mutex> lockAskOrders(bestAsk->mtx);
                    std::unique_lock<std::mutex> lockBidOrders(bestBid->mtx);

                    // Unlock shared locks early to allow other operations
                    lockBidsShared.unlock();
                    lockAsksShared.unlock(); // We have traversed the order trees to get the limit objects we need, so we can allow threads to add more orders
                    
                    // Retrieve orders from best ask and bid
                    std::deque<std::shared_ptr<OrderInternal>>& askOrders = bestAsk->orders;
                    std::deque<std::shared_ptr<OrderInternal>>& bidOrders = bestBid->orders;
                    
                    // Iterate through the orders to match
                    while (!bestAsk->orders.empty() && !bestBid->orders.empty() && bestAsk->orders.front()->price <= bestBid->orders.front()->price) {
                        // Match orders
                        std::shared_ptr<OrderInternal> askOrder = bestAsk->orders.front();
                        std::shared_ptr<OrderInternal> bidOrder = bestBid->orders.front();
                        
                        // Update order book
                        // Remove matched orders from order book
                        double fillPrice = (askOrder->price + bidOrder->price) / 2.0;
                        // Update partially filled order if necessary
                        if (askOrder->size > bidOrder->size) {
                            std::cout << "          Matched order: " << bidOrder->orderId << ": BID " << bidOrder->size << " shares at $" << fillPrice << std::endl;
                            std::cout << "Partially matched order: " << askOrder->orderId << ": ASK " << bidOrder->size << " shares at $" << fillPrice << std::endl;
                            askOrder->size -= bidOrder->size;
                            orderbook.volume += bidOrder->size;
                            bidOrders.pop_front(); // Fully filled
                            //sendTradeNotification(bidOrder);
                            
                        } else if (askOrder->size < bidOrder->size) {
                            std::cout << "          Matched order: " << askOrder->orderId << ": ASK " << askOrder->size << " shares at $" << fillPrice << std::endl;
                            std::cout << "Partially matched order: " << bidOrder->orderId << ": ASK " << askOrder->size << " shares at $" << fillPrice << std::endl;
                            bidOrder->size -= askOrder->size;
                            orderbook.volume += askOrder->size;
                            askOrders.pop_front(); // Filly filled
                            //sendTradeNotification(askOrder);
                        } else {
                            // Orders are fully matched, so do not need to be added back to the order book
                            std::cout << "Matched order: " << askOrder->orderId << ": ASK " << askOrder->size << " shares at $" << fillPrice << std::endl;
                            std::cout << "Matched order: " << bidOrder->orderId << ": BID " << bidOrder->size << " shares at $" << fillPrice << std::endl;
                            askOrders.pop_front(); // Filly filled
                            bidOrders.pop_front(); // Fully filled
                            orderbook.volume += bidOrder->size;
                            //sendTradeNotification(askOrder);
                            //sendTradeNotification(bidOrder);
                        }
                    }
                    // At this point, one or both LimitPrices have been exhausted, so they should be deleted from the book
                    if (askOrders.empty()) {
                        std::cout << "Thread ID" << std::this_thread::get_id() << " Old Ask: " << ask << std::endl;
                        std::unique_lock<std::shared_mutex> lockAsksUnique(askMutex);
                        lockAskOrders.unlock();
                        orderbook.askTree.erase(ask);
                        double newAsk = orderbook.refreshAskPointer(); // refreshAsk
                        std::cout << "Thread ID" << std::this_thread::get_id() << " New Ask: " << (newAsk != 0.0 ? std::to_string(newAsk) : "nullptr") << std::endl;
                    }
                    if (bidOrders.empty()) {
                        std::cout << "Thread ID" << std::this_thread::get_id() << " Old Bid: " << bid << std::endl;
                        std::unique_lock<std::shared_mutex> lockBidsUnique(bidMutex);
                        lockBidOrders.unlock();
                        orderbook.bidTree.erase(bid);
                        double newBid = orderbook.refreshBidPointer();
                        std::cout << "Thread ID" << std::this_thread::get_id() << " New Bid: " << (newBid != 0.0 ? std::to_string(newBid) : "nullptr") << std::endl;
                    }
                    if (timerShutoffMode && orderbook.ask == nullptr && orderbook.bid == nullptr && timerStarted ) {
                        auto endTime = std::chrono::steady_clock::now();
                        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
                        std::cout << "Matching completed in " << duration << " milliseconds." << std::endl;
                        return; // Stop the algorithm since we are done
                    }
                } else if (testFlag && ++consecutiveIterationsWithoutMatch >= 20) {
                    return;
                }
            }
        }
        if (++iterationCount % 10 == 0) {
            {
                std::shared_lock<std::shared_mutex> lockAsksShared(askMutex);
                std::shared_lock<std::shared_mutex> lockBidsShared(bidMutex);
                double bid = orderbook.getBid();
                double ask = orderbook.getAsk();
                std::cout << "Thread ID " << this_id << ": " << "Bid: " << bid << ", Ask: " << ask << ", Spread: " << (ask - bid) << std::endl;
            }
        }
        if (testFlag && ++consecutiveIterationsWithNoAskOrBid >= 20) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::cout << "Thread ID " << this_id << ": " << orderbook.security << " Matching algorithm halted..." << std::endl;
}
