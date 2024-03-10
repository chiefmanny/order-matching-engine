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
#include "../include/matching_algorithm.h"

using namespace std;

MatchingAlgorithm::MatchingAlgorithm(OrderBook& orderbook_in, std::shared_mutex& askMutex_in, std::shared_mutex& bidMutex_in) : orderbook(orderbook_in), askMutex(askMutex_in), bidMutex(bidMutex_in) {}

void MatchingAlgorithm::run() {
    while (true) {
        {
            std::unique_lock<std::shared_mutex> lockBids(bidMutex);
            std::unique_lock<std::shared_mutex> lockAsks(askMutex);
            if (orderbook.getAsk() <= orderbook.getBid()) {
                // Retrieve best ask and bid orders
                const double ask = orderbook.getAsk();
                const double bid = orderbook.getBid();
                
                LimitPrice* bestAsk = &(orderbook.askTree.begin()->second);
                LimitPrice* bestBid = &(orderbook.bidTree.begin()->second);
                
                // Retrieve orders from best ask and bid
                std::deque<std::shared_ptr<Order>>& askOrders = bestAsk->orders;
                std::deque<std::shared_ptr<Order>>& bidOrders = bestBid->orders;
                
                // Iterate through the orders to match
                while (!askOrders.empty() && !bidOrders.empty() && askOrders.front()->price <= bidOrders.front()->price) {
                    // Match orders
                    std::shared_ptr<Order> askOrder = askOrders.front();
                    std::shared_ptr<Order> bidOrder = bidOrders.front();
                    
                    // Update order book
                    // Remove matched orders from order book
                    
                    // Update partially filled order if necessary
                    if (askOrder->size > bidOrder->size) {
                        std::cout << "Matched order: " << bidOrder->orderId << ": BID " << bidOrder->size << " shares at $" << bidOrder->price << std::endl;
                        askOrder->size -= bidOrder->size;
                        orderbook.volume += bidOrder->size;
                        bidOrders.pop_front(); // Fully filled
                        //sendTradeNotification(bidOrder);
                        
                    } else if (askOrder->size < bidOrder->size) {
                        std::cout << "Matched order: " << askOrder->orderId << ": ASK " << askOrder->size << " shares at $" << askOrder->price << std::endl;
                        bidOrder->size -= askOrder->size;
                        orderbook.volume += askOrder->size;
                        askOrders.pop_front(); // Filly filled
                        //sendTradeNotification(askOrder);
                    } else {
                        // Orders are fully matched, so do not need to be added back to the order book
                        std::cout << "Matched order: " << askOrder->orderId << ": ASK " << askOrder->size << " shares at $" << askOrder->price << std::endl;
                        std::cout << "Matched order: " << bidOrder->orderId << ": BID " << bidOrder->size << " shares at $" << bidOrder->price << std::endl;
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
                    orderbook.askTree.erase(ask);
                    orderbook.ask = &(orderbook.askTree.begin()->second); // refreshAsk
                }
                if (bidOrders.empty()) {
                    orderbook.bidTree.erase(bid);
                    orderbook.bid = &(orderbook.bidTree.begin()->second); // refreshBid
                }
            }
        }
        // Sleep for a certain period before checking again (to prevent busy waiting)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
