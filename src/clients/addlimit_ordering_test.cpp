#include "../grpc/matching_engine_client.h"

#include <chrono>

int main(int argc, char** argv) {
    MatchingEngineClient client1("localhost:50051");
    MatchingEngineClient client2("localhost:50051");
    MatchingEngineClient client3("localhost:50051");

    auto watchThread = [&client1]() {
        client1.watchSymbol_print("AAPL", 60, true);
    };

    auto orderThread = [&client2]() {
        client2.addOrder("AAPL", 50.0, 100, false);
    };

    auto orderThread2 = [&client3]() {
        client3.addOrder("AAPL", 60.0, 100, true);
    };

    std::thread watcher(watchThread);
    std::thread orderer(orderThread);
    std::thread orderer2(orderThread2);

    // Join the threads
    watcher.join();
    orderer.join();
    orderer2.join();

    return 0;
}