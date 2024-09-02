#include "matching_engine_client.h"

#include <chrono>
#include <random>
#include <vector>
#include <iostream>
#include <thread>

using namespace std;

void measureLatency(MatchingEngineClient& client, const std::string& symbol, double price, unsigned int size, bool buy, std::vector<long long>& latencies) {
    auto start = std::chrono::steady_clock::now();
    unsigned int orderId = client.addOrder(symbol, price, size, buy);
    auto end = std::chrono::steady_clock::now();

    if (orderId > 0) {
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        latencies.push_back(duration);
    } else {
        std::cerr << "Order failed to execute." << std::endl;
    }
}

void runLatencyTests(MatchingEngineClient& client, const std::string& symbol, int numTests) {
    std::vector<long long> latencies;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::normal_distribution<> buyDist(50, 5); // mean = 50, stddev = 3
    std::normal_distribution<> sellDist(100, 5); // mean = 60, stddev = 3

    // Cold start: Run a few orders to "warm up" the server (not measured)
    for (int i = 0; i < 5; ++i) {
        double price = buyDist(gen);
        client.addOrder(symbol, price, 100, true);
    }

    // Actual latency measurements
    for (int i = 0; i < numTests; ++i) {
        double price = (i % 2 == 0) ? buyDist(gen) : sellDist(gen);
        bool buy = (i % 2 == 0);
        measureLatency(client, symbol, price, 100, buy, latencies);
    }

    // Calculate average latency
    long long sum = 0;
    for (auto latency : latencies) {
        sum += latency;
    }
    long long avgLatency = sum / latencies.size();

    // Output results
    std::cout << "Average Latency over " << numTests << " tests: " << avgLatency << " microseconds" << std::endl;
    std::cout << "Latency measurements (microseconds):" << std::endl;
    for (const auto& latency : latencies) {
        std::cout << latency << " ";
    }
    std::cout << std::endl;
}

int main(int argc, char** argv) {
    MatchingEngineClient client("localhost:50051");

    const std::string symbol = "AAPL";
    const int numTests = 10000; // Number of latency measurements

    std::cout << "Starting latency tests..." << std::endl;
    runLatencyTests(client, symbol, numTests);
    std::cout << "Latency tests completed." << std::endl;

    return 0;
}
