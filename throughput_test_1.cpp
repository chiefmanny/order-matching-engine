#include "matching_engine_client.h"

#include <random>
#include <queue>
#include <thread>
#include <vector>
#include <iostream>
#include <atomic>
#include <chrono>

using namespace std;

void runThroughputTests(MatchingEngineClient& buyerclient, MatchingEngineClient& sellerclient, const std::string& symbol, int numTests) {
    std::queue<double> buyOrders; // queue to store buy order prices, size will always be 100
    std::mutex queueMutex; // to protect access to the buyOrders queue
    std::atomic<int> ordersPlaced{0}; // track number of orders placed
    std::atomic<int> ordersMatched{0}; // track number of orders matched
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::normal_distribution<> buyDist(50, 5); // mean = 50, stddev = 5

    auto buyerThread = [&]() {
        for (int i = 0; i < numTests; ++i) {
            double price = numTests + i;
            unsigned int orderId = buyerclient.addOrder(symbol, price, 100, true); // buy order
            std::cout << "Order added: Buy 100 shares of AAPL at " << price << std::endl;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                buyOrders.push(i);
            }
            ordersPlaced++;
        }
    };

    auto sellerThread = [&]() {
        while (ordersPlaced < numTests || !buyOrders.empty()) { // Keep running until all buy orders are placed and matched
            double price;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (!buyOrders.empty()) {
                    price = numTests - buyOrders.front();
                    buyOrders.pop();
                } else {
                    continue; // If the queue is empty, wait for more buy orders
                }
            }
            unsigned int orderId = sellerclient.addOrder(symbol, price, 100, false); // sell order
            std::cout << "Order added: Sell 100 shares of AAPL at " << price << std::endl;
            ordersMatched++;
        }
    };

    // Start the throughput test
    auto startTime = std::chrono::steady_clock::now();

    // Launch buyer thread
    std::thread buyer(buyerThread);

    // Launch seller thread
    std::thread seller(sellerThread);

    // Wait for both threads to finish
    buyer.join();
    seller.join();

    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    // Output the test results
    std::cout << "Throughput test completed in " << duration << " milliseconds." << std::endl;
    std::cout << "Total Orders Placed: " << ordersPlaced << std::endl;
    std::cout << "Total Orders Matched: " << ordersMatched << std::endl;
}

int main(int argc, char** argv) {
    MatchingEngineClient buyerclient("localhost:50051");
    MatchingEngineClient sellerclient("localhost:50051");

    const std::string symbol = "AAPL";
    const int numTests = 100; // Number of orders to place

    std::cout << "Starting throughput tests..." << std::endl;
    runThroughputTests(buyerclient, sellerclient, symbol, numTests);
    std::cout << "Throughput tests completed." << std::endl;

    return 0;
}
