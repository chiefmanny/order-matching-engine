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
            ordersMatched++;
        }
    };

    // Start the throughput test
    auto startTime = std::chrono::steady_clock::now();

    // Launch buyer thread
    std::thread buyer(buyerThread);

    std::this_thread::sleep_for(std::chrono::seconds(1));

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
    std::cout << "Test Duration: " << duration << " ms\n"; // Output duration
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <numTests>" << std::endl;
        return 1;
    }

    int numTests = atoi(argv[1]);

    MatchingEngineClient buyerclient("localhost:50051");
    MatchingEngineClient sellerclient("localhost:50051");

    const std::string symbol = "AAPL";

    std::cout << "Starting throughput tests with " << numTests << " orders..." << std::endl;
    runThroughputTests(buyerclient, sellerclient, symbol, numTests);
    std::cout << "Throughput tests completed." << std::endl;

    return 0;
}
