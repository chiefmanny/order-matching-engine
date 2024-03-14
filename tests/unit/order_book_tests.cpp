//
//  order_book_tests.cpp
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 3/10/24.
//

#include "unit_tests.h"
#include "../../backend/include/order_book.h"
#include "../../backend/include/matching_algorithm.h"
#include <stdio.h>
#include <shared_mutex>
#include <mutex>
#include <cassert>
#include <set>
#include <thread>

void orderbook_basic_test_1() {
    std::shared_mutex ask;
    std::shared_mutex bid;
    OrderBook book("GME", 1234567890, ask, bid);
    
    std::vector<unsigned int> orderIds;
    
    orderIds.push_back(book.addLimitOrder(10.5, 10, true));
    orderIds.push_back(book.addLimitOrder(11.5, 10, true));
    orderIds.push_back(book.addLimitOrder(12.5, 10, true)); // bids
    orderIds.push_back(book.addLimitOrder(13.5, 10, true));
    orderIds.push_back(book.addLimitOrder(14.5, 10, true));
    
    orderIds.push_back(book.addLimitOrder(14.5, 10, false));
    orderIds.push_back(book.addLimitOrder(15.5, 10, false));
    orderIds.push_back(book.addLimitOrder(16.5, 10, false)); // asks
    orderIds.push_back(book.addLimitOrder(17.5, 10, false));
    orderIds.push_back(book.addLimitOrder(18.5, 10, false));
    
    assert(book.allBuyOrders.size() == 5);
    assert(book.allSellOrders.size() == 5);
    std::set<unsigned int> uniqueIds(orderIds.begin(), orderIds.end());
    assert(uniqueIds.size() == 10);
    
    assert(book.getAsk() == 14.5);
    assert(book.getBid() == 14.5);
    
    assert(book.askTree[14.5].totalSize == 10);
    assert(book.askTree[14.5].totalSize == 10);
    
    book.cancelLimitOrder(orderIds[5]);
    book.cancelLimitOrder(orderIds[4]);
    
    assert(!book.askTree.contains(14.5));
    assert(!book.bidTree.contains(14.5));
    
    assert(book.getAsk() == 15.5);
    assert(book.getBid() == 13.5);
    
    orderIds.push_back(book.addLimitOrder(15.5, 15, false));
    orderIds.push_back(book.addLimitOrder(15.5, 25, false));
    orderIds.push_back(book.addLimitOrder(15.5, 50, false));
    orderIds.push_back(book.addLimitOrder(15.5, 150, false));
    
    assert(book.volume == 0);
    
    assert(book.askTree[15.5].totalSize == 250);
}

void orderbook_multithread_test_1() {
    std::shared_mutex ask;
    std::shared_mutex bid;
    OrderBook book("GME", 1234567890, ask, bid);
    MatchingAlgorithm algo(book, ask, bid);
    
    // Function to add an order to the book
    auto addOrder = [&](double price, unsigned int size, bool buy) {
        book.addLimitOrder(price, size, buy);
    };
    
    // Create threads for each addLimitOrder call
    std::vector<std::thread> threads;
    threads.emplace_back(addOrder, 10.5, 10, true);
    threads.emplace_back(addOrder, 11.5, 10, true);
    threads.emplace_back(addOrder, 12.5, 10, true);
    threads.emplace_back(addOrder, 13.5, 10, true);
    threads.emplace_back(addOrder, 14.5, 10, true);
    threads.emplace_back(addOrder, 14.5, 10, false);
    threads.emplace_back(addOrder, 15.5, 10, false);
    threads.emplace_back(addOrder, 16.5, 10, false);
    threads.emplace_back(addOrder, 17.5, 10, false);
    threads.emplace_back(addOrder, 18.5, 10, false);
    
    // Perform matching
    std::thread algoThread([&]() {
        algo.run(true);
    });
    
    // Join threads to wait for them to finish
    for (auto& thread : threads) {
        thread.join();
    }
        
    algoThread.join(); // Will never return if run is called with false
}

void orderbook_multithread_test_2() {
    std::shared_mutex ask;
    std::shared_mutex bid;
    OrderBook book("GME", 1234567890, ask, bid);
    MatchingAlgorithm algo(book, ask, bid);
    
    auto addOrder = [&](double price, unsigned int size, bool buy) {
        book.addLimitOrder(price, size, buy);
    };
    
    std::vector<std::thread> threads;
    threads.emplace_back(addOrder, 14.5, 457434, true);
    threads.emplace_back(addOrder, 14.5, 356345, true);
    threads.emplace_back(addOrder, 14.5, 6435433, true);
    threads.emplace_back(addOrder, 14.5, 25213213, true);
    threads.emplace_back(addOrder, 14.5, 123212355, true);
    threads.emplace_back(addOrder, 14.5, 457434, false);
    threads.emplace_back(addOrder, 14.5, 356345, false);
    threads.emplace_back(addOrder, 14.5, 6435433, false);
    threads.emplace_back(addOrder, 14.5, 25213213, false);
    threads.emplace_back(addOrder, 14.5, 123212355, false);
    
    std::thread algoThread([&]() {
        algo.run(true);
    });
    
    for (auto& thread : threads) {
        thread.join();
    }
        
    algoThread.join();
}
