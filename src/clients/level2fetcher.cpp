#include "../grpc/matching_engine_client.h"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    MatchingEngineClient client("localhost:50051");

    // Total duration in seconds
    int totalDuration = 60;
    // Interval to fetch data in seconds
    int interval = 10;
    // Depth of the order book to fetch
    int depth = 5;
    // Symbol to fetch data for
    std::string symbol = "AAPL";

    for (int i = 0; i < totalDuration; i += interval) {
        std::cout << "Fetching level 2 data for " << symbol << " (Depth: " << depth << ")" << std::endl;

        // Fetch both bid and ask data
        client.getLevel2(symbol, depth, true, matchingengine::Level2Request::BOTH);

        // Sleep for the interval
        std::this_thread::sleep_for(std::chrono::seconds(interval));
    }

    std::cout << "Finished fetching level 2 data for " << symbol << std::endl;
    return 0;
}
