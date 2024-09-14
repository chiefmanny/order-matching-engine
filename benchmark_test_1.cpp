#include "matching_engine_client.h"

#include <chrono>
#include <random>
#include <vector>
#include <iostream>

using namespace std;

int buys = 0;
int sells = 0;
int cancels = 0;

int main(int argc, char** argv) {
    MatchingEngineClient client("localhost:50051");

    std::vector<unsigned int> buyOrderIDs;
    std::vector<unsigned int> sellOrderIDs;

    auto watchThread = [&client]() {
        std::cout << "Watch thread starting" << std::endl;
        client.watchSymbol_print("AAPL", 30, true);
        std::cout << "Watch thread complete" << std::endl;
    };

    auto buyOrderThread = [&client, &buyOrderIDs]() {
        std::cout << "Buy order thread starting" << std::endl;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::normal_distribution<> d(50, 3); // mean = 50, stddev = 3

        auto startTime = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - startTime < std::chrono::seconds(20)) {
            double price = d(gen);
            unsigned int orderID = client.addOrder("AAPL", price, 100, true); // buy order
            buyOrderIDs.push_back(orderID);
            buys++;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::cout << "Buy order thread complete" << std::endl;
    };

    auto sellOrderThread = [&client, &sellOrderIDs]() {
        std::cout << "Sell order thread starting" << std::endl;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::normal_distribution<> d(60, 3); // mean = 60, stddev = 3

        auto startTime = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - startTime < std::chrono::seconds(20)) {
            double price = d(gen);
            unsigned int orderID = client.addOrder("AAPL", price, 100, false); // sell order
            sellOrderIDs.push_back(orderID);
            sells++;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::cout << "Sell order thread complete" << std::endl;
    };

    auto cancelOrderThread = [&client, &buyOrderIDs, &sellOrderIDs]() {
        std::cout << "Cancel order thread starting" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(5)); // wait 5 seconds before cancelling
        for (const auto& orderID : buyOrderIDs) {
            std::cout << "Buy ID: " << orderID << " " << std::endl;
            if (client.cancelOrder("AAPL", orderID) == 0) {
                std::cout << "Failed to cancel: " << orderID << " " << std::endl;
            }
            else {
                std::cout << "Successfully cancelled: " << orderID << " " << std::endl;
                cancels++;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (const auto& orderID : sellOrderIDs) {
            std::cout << "Sell ID: " << orderID << " " << std::endl;
            if (client.cancelOrder("AAPL", orderID) == 0) {
                std::cout << "Failed to cancel: " << orderID << " " << std::endl;
            }
            else {
                std::cout << "Successfully cancelled: " << orderID << " " << std::endl;
                cancels++;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::cout << "Cancel order thread complete" << std::endl;
    };

    std::thread watcher(watchThread);
    std::thread buyOrderer(buyOrderThread);
    std::thread sellOrderer(sellOrderThread);

    buyOrderer.join();
    sellOrderer.join();

    //std::thread canceller(cancelOrderThread);

    watcher.join();
    //canceller.join();

    std::cout << "Buys: " << buys << std::endl;
    std::cout << "Sells: " << sells << std::endl;
    //std::cout << "Cancels: " << cancels << std::endl;

    return 0;
}
