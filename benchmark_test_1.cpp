#include "matching_engine_client.h"

#include <chrono>
#include <random>
#include <vector>

int main(int argc, char** argv) {
    MatchingEngineClient client("localhost:50051");

    std::vector<unsigned int> buyOrderIDs;
    std::vector<unsigned int> sellOrderIDs;

    auto watchThread = [&client]() {
        client.watchSymbol("AAPL", 60, true);
    };

    auto buyOrderThread = [&client, &buyOrderIDs]() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::normal_distribution<> d(50, 3); // mean = 50, stddev = 3

        auto startTime = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - startTime < std::chrono::seconds(20)) {
            double price = d(gen);
            unsigned int orderID = client.addOrder("AAPL", price, 100, true); // buy order
            buyOrderIDs.push_back(orderID);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    auto sellOrderThread = [&client, &sellOrderIDs]() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::normal_distribution<> d(60, 3); // mean = 60, stddev = 3

        auto startTime = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - startTime < std::chrono::seconds(20)) {
            double price = d(gen);
            unsigned int orderID = client.addOrder("AAPL", price, 100, false); // sell order
            sellOrderIDs.push_back(orderID);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    auto cancelOrderThread = [&client, &buyOrderIDs, &sellOrderIDs]() {
        std::this_thread::sleep_for(std::chrono::seconds(5)); // wait 5 seconds before cancelling
        for (const auto& orderID : buyOrderIDs) {
            client.cancelOrder("AAPL", orderID);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (const auto& orderID : sellOrderIDs) {
            client.cancelOrder("AAPL", orderID);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    std::thread watcher(watchThread);
    std::thread buyOrderer(buyOrderThread);
    std::thread sellOrderer(sellOrderThread);

    buyOrderer.join();
    sellOrderer.join();

    std::thread canceller(cancelOrderThread);

    watcher.join();
    canceller.join();

    return 0;
}
