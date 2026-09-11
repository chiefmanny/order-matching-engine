//
//  order_book_tests.cpp
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 3/10/24.
//

#include "unit_tests.h"
#include "order_book.h"
#include "matching_algorithm.h"
#include <stdio.h>
#include <shared_mutex>
#include <mutex>
#include <cassert>
#include <set>
#include <thread>
#include <iostream>
#include <random>

void orderbook_basic_test_1() {
    std::cout << "Running test: " << __func__ << "..." << std::endl;
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
    
    assert(book.askTree.find(14.5) == book.askTree.end());
    assert(book.bidTree.find(14.5) == book.bidTree.end());
    
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
    std::cout << "Running test: " << __func__ << "..." << std::endl;
    std::shared_mutex ask;
    std::shared_mutex bid;
    OrderBook book("GME", 1234567890, ask, bid);
    MatchingAlgorithm algo(book, ask, bid);
    std::atomic<bool> running{true};
    
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
        algo.run(running, true, false);
    });
    
    // Join threads to wait for them to finish
    for (auto& thread : threads) {
        thread.join();
    }
        
    algoThread.join(); // Will never return if run is called with false
}

void orderbook_multithread_test_2() {
    std::cout << "Running test: " << __func__ << "..." << std::endl;
    std::shared_mutex ask;
    std::shared_mutex bid;
    OrderBook book("GME", 1234567890, ask, bid);
    MatchingAlgorithm algo(book, ask, bid);
    std::atomic<bool> running{true};
    
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
        algo.run(running, true, false);
    });
    
    for (auto& thread : threads) {
        thread.join();
    }
        
    algoThread.join();
}

void orderbook_multithread_test_3() {
    std::cout << "Running test: " << __func__ << "..." << std::endl;
    std::shared_mutex ask;
    std::shared_mutex bid;
    OrderBook book("GME", 1234567890, ask, bid);
    MatchingAlgorithm algo(book, ask, bid);
    std::atomic<bool> running{true};
    
    auto addOrder = [&](double price, unsigned int size, bool buy) {
        book.addLimitOrder(price, size, buy);
    };
    
    std::vector<std::thread> threads;
    threads.emplace_back(addOrder, 14.5, 457434, true);
    threads.emplace_back(addOrder, 14.5, 457434, false);
    
    std::thread algoThread([&]() {
        algo.run(running, true, false);
    });
    
    for (auto& thread : threads) {
        thread.join();
    }
        
    algoThread.join();
}

void randomized_orderbook_test() {
    std::cout << "Running test: " << __func__ << "..." << std::endl;

    std::shared_mutex ask;
    std::shared_mutex bid;
    OrderBook book("GME", 1234567890, ask, bid);
    MatchingAlgorithm algo(book, ask, bid);
    std::atomic<bool> running{true};

    // Function to add an order to the book
    auto addRandomOrder = [&](unsigned int count) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<double> price_dist(10.0, 20.0);
        std::uniform_int_distribution<unsigned int> size_dist(1, 1000);
        std::uniform_int_distribution<int> buy_dist(0, 1);

        for (unsigned int i = 0; i < count; ++i) {
            double price = price_dist(gen);
            unsigned int size = size_dist(gen);
            bool buy = buy_dist(gen);
            book.addLimitOrder(price, size, buy);
        }
    };

    // Create threads for adding random orders
    std::vector<std::thread> threads;
    threads.emplace_back(addRandomOrder, 1000);
    threads.emplace_back(addRandomOrder, 1000);
    threads.emplace_back(addRandomOrder, 1000);
    threads.emplace_back(addRandomOrder, 1000);
    threads.emplace_back(addRandomOrder, 1000);

    // Sleep to allow some orders to be added before starting the matching algorithm
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Start the matching algorithm
    std::thread algoThread([&]() {
        algo.run(running, true, false); // Run in testing mode
    });

    // Continue adding random orders while the algorithm is running
    for (unsigned int i = 0; i < 5; ++i) {
        addRandomOrder(1000);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Join threads to wait for them to finish
    for (auto& thread : threads) {
        thread.join();
    }

    // Join the matching algorithm thread
    algoThread.join();
}

// Helper for next test

void generateSkewedOrders(OrderBook& book, unsigned int count, double midPrice, unsigned int maxSpread) {
    std::random_device rd;
    std::mt19937 gen(rd());
    
    // Generate buy orders skewed to the right
    std::normal_distribution<double> buyPriceDist(midPrice, maxSpread / 3.0);
    for (unsigned int i = 0; i < count; ++i) {
        double price = std::max(midPrice, buyPriceDist(gen));
        book.addLimitOrder(price, 1, true);
    }
    
    // Generate sell orders skewed to the left
    std::normal_distribution<double> sellPriceDist(midPrice, maxSpread / 3.0);
    for (unsigned int i = 0; i < count; ++i) {
        double price = std::min(midPrice, sellPriceDist(gen));
        book.addLimitOrder(price, 1, false);
    }
}

void skewed_orderbook_test(double midPrice, unsigned int maxSpread) {
    std::cout << "Running test: " << __func__ << "..." << std::endl;

    std::shared_mutex ask;
    std::shared_mutex bid;
    OrderBook book("GME", 1234567890, ask, bid);
    MatchingAlgorithm algo(book, ask, bid);
    std::atomic<bool> running{true};

    // Generate skewed orders
    generateSkewedOrders(book, 1000, midPrice, maxSpread);

    // Sleep to allow some orders to be added before starting the matching algorithm
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Start the matching algorithm
    std::thread algoThread([&]() {
        algo.run(running, true, false); // Run in testing mode
    });

    // Continue adding skewed orders while the algorithm is running
    for (unsigned int i = 0; i < 5; ++i) {
        generateSkewedOrders(book, 1000, midPrice, maxSpread);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Join the matching algorithm thread
    algoThread.join();
}


void randomized_orderbook_test_buy_trend() {
    std::cout << "Running test: " << __func__ << "..." << std::endl;

    std::shared_mutex ask;
    std::shared_mutex bid;
    OrderBook book("GME", 1234567890, ask, bid);
    MatchingAlgorithm algo(book, ask, bid);
    std::atomic<bool> running{true};
    
    const unsigned int sharesPerOrder = 100;

    // Function to add an order to the book
    auto addRandomOrder = [&](unsigned int count, double buyRatio) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<double> price_dist(10.0, 20.0);
        std::uniform_int_distribution<unsigned int> size_dist(1, 1000);

        // Adjust the ratio of buy to sell orders
        unsigned int buyCount = static_cast<unsigned int>(count * buyRatio);
        unsigned int sellCount = count - buyCount;

        // Add buy orders
        for (unsigned int i = 0; i < buyCount; ++i) {
            double price = price_dist(gen);
            book.addLimitOrder(price, sharesPerOrder, true);
        }

        // Add sell orders
        for (unsigned int i = 0; i < sellCount; ++i) {
            double price = price_dist(gen);
            book.addLimitOrder(price, sharesPerOrder, false);
        }
    };

    // Initial buy ratio
    double buyRatio = 0.6;

    // Create threads for adding random orders
    std::vector<std::thread> threads;
    for (unsigned int i = 0; i < 5; ++i) {
        threads.emplace_back(addRandomOrder, 1000, buyRatio);
        buyRatio += 0.05;  // Increase buy ratio for each batch
    }

    // Sleep to allow some orders to be added before starting the matching algorithm
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Start the matching algorithm
    std::thread algoThread([&]() {
        algo.run(running, true, false); // Run in testing mode
    });

    // Continue adding random orders while the algorithm is running
    for (unsigned int i = 0; i < 5; ++i) {
        addRandomOrder(1000, buyRatio);
        buyRatio += 0.05;  // Increase buy ratio for each batch
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Join threads to wait for them to finish
    for (auto& thread : threads) {
        thread.join();
    }

    // Join the matching algorithm thread
    algoThread.join();
}


void massive_orderbook_test_price_movement() {
    std::cout << "Running test: " << __func__ << "..." << std::endl;

    // Mutexes for ask and bid
    std::shared_mutex ask;
    std::shared_mutex bid;

    // Initialize OrderBook and MatchingAlgorithm
    OrderBook book("GME", 1234567890, ask, bid);
    MatchingAlgorithm algo(book, ask, bid);
    std::atomic<bool> running{true};

    // Function to add an order to the book
    auto addOrder = [&](double price, unsigned int size, bool buy) {
        book.addLimitOrder(price, size, buy);
    };

    // Random number generation for prices
    std::default_random_engine generator;
    std::uniform_real_distribution<double> distribution(10.0, 20.0);

    // Vector to store threads
    std::vector<std::thread> threads;

    // Initial orders trading around low 10's
    for (int i = 0; i < 100; ++i) {
        threads.emplace_back(addOrder, distribution(generator), 1000, true);
        threads.emplace_back(addOrder, distribution(generator), 1000, false);
    }

    // Simulate gradual increase in price
    for (double price = 10.1; price < 19.9; price += 0.1) {
        threads.emplace_back(addOrder, price, 1000, true);
        threads.emplace_back(addOrder, price, 1000, false);
    }

    // Simulate gradual decrease in price
    for (double price = 19.9; price > 10.0; price -= 0.1) {
        threads.emplace_back(addOrder, price, 1000, true);
        threads.emplace_back(addOrder, price, 1000, false);
    }

    // Perform matching
    std::thread algoThread([&]() {
        algo.run(running, true, false);
    });

    // Join threads to wait for them to finish
    for (auto& thread : threads) {
        thread.join();
    }

    // Join algoThread
    algoThread.join();
}
