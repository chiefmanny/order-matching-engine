#include "matching_engine_client.h"

#include <chrono>

int main(int argc, char** argv) {
    MatchingEngineClient client("localhost:50051");

    auto watchThread = [&client]() {
        client.watchSymbol_print("AAPL", 60, true);
    };


    auto orderThread = [&client]() {
        for (int i = 100; i > -100; i-=0.0001) {
            client.addOrder("AAPL", 150 + i, 100, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            client.addOrder("AAPL", 150 - i, 100, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };

    std::thread watcher(watchThread);
    std::thread orderer(orderThread);

    // Join the threads
    watcher.join();
    orderer.join();

    return 0;

}