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

void MatchingAlgorithm::run(std::atomic<bool>& running, bool testFlag = false) {
    int consecutiveIterationsWithoutMatch = 0;
    int consecutiveIterationsWithNoAskOrBid = 0;
    int iterationCount = 0;
    std::thread::id this_id = std::this_thread::get_id();
    std::cout << "Thread ID " << this_id << ": Running " << orderbook.security << " matching algorithm..." << std::endl;
    while (running) {
        std::unique_lock<std::shared_mutex> lockBids(bidMutex, std::defer_lock);
        std::unique_lock<std::shared_mutex> lockAsks(askMutex, std::defer_lock);

        std::lock(lockBids, lockAsks);  // Lock both mutexes simultaneously
        if (!orderbook.askEmpty() && !orderbook.bidEmpty()) {
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
                        orderbook.volume += askOrder->size;
                        orderbook.volume += bidOrder->size;
                        //sendTradeNotification(askOrder);
                        //sendTradeNotification(bidOrder);
                    }
                }
                // At this point, one or both LimitPrices have been exhausted, so they should be deleted from the book
                if (askOrders.empty()) {
                    std::cout << "Thread ID" << std::this_thread::get_id() << " Old Ask: " << ask << std::endl;
                    orderbook.askTree.erase(ask);
                    orderbook.ask = &(orderbook.askTree.begin()->second); // refreshAsk
                    std::cout << "Thread ID" << std::this_thread::get_id() << " New Ask: " << (orderbook.askTree.begin()->first) << std::endl;
                }
                if (bidOrders.empty()) {
                    std::cout << "Thread ID" << std::this_thread::get_id() << " Old Bid: " << bid << std::endl;
                    orderbook.bidTree.erase(bid);
                    orderbook.bid = &(orderbook.bidTree.begin()->second); // refreshBid
                    std::cout << "Thread ID" << std::this_thread::get_id() << " New Bid: " << (orderbook.bidTree.begin()->first) << std::endl;
                }
            } else if (testFlag && ++consecutiveIterationsWithoutMatch >= 20) {
                return;
            }
            if (++iterationCount % 10 == 0) {
                double bid = orderbook.getBid();
                double ask = orderbook.getAsk();
                std::cout << "Thread ID " << this_id << ": " << "Bid: " << bid << ", Ask: " << ask << ", Spread: " << (ask - bid) << std::endl;
            }
        }
        if (testFlag && ++consecutiveIterationsWithNoAskOrBid >= 20) return;
        // Sleep for a certain period before checking again (to prevent busy waiting)
        lockBids.unlock();
        lockAsks.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "Thread ID " << this_id << ": " << orderbook.security << " Matching algorithm halted..." << std::endl;
}
