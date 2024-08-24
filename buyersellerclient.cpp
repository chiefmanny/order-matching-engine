#include "matching_engine_client.h"

int main(int argc, char** argv) {
    MatchingEngineClient client("localhost:50051");

    // Synchronization variables
    std::mutex cv_mtx;
    std::condition_variable cv;
    bool buyerReady = false;
    bool sellerReady = false;

    // Thread function for buyer
    auto buyerThread = [&client, &cv_mtx, &cv, &buyerReady]() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::normal_distribution<> priceChange(0.005, 0.002); // small incremental increase with a bit of variance
        double currentPrice = 150.0; // Starting price
        for (int i = 0; i < 1000; ++i) {
            unsigned int size = 100 + (rand() % 100); // Random size between 100 and 200
            currentPrice += priceChange(gen);
            client.addOrder("AAPL", currentPrice, size, true); // Buyer thread adds buy orders

            if (i == 0) {
                // Notify watcher that the first order is placed
                std::lock_guard<std::mutex> lock(cv_mtx);
                buyerReady = true;
                cv.notify_all();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Delay between orders
        }
    };

    // Thread function for seller
    auto sellerThread = [&client, &cv_mtx, &cv, &sellerReady]() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::normal_distribution<> priceChange(-0.005, 0.002); // small incremental decrease with a bit of variance
        double currentPrice = 151.0; // Starting price
        for (int i = 0; i < 1000; ++i) {
            unsigned int size = 100 + (rand() % 100); // Random size between 100 and 200
            currentPrice += priceChange(gen);
            client.addOrder("AAPL", currentPrice, size, false); // Seller thread adds sell orders

            if (i == 0) {
                // Notify watcher that the first order is placed
                std::lock_guard<std::mutex> lock(cv_mtx);
                sellerReady = true;
                cv.notify_all();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Delay between orders
        }
    };

    // Thread function for watching the symbol after both buyer and seller have submitted their first orders
    auto watchThread = [&client, &cv_mtx, &cv, &buyerReady, &sellerReady]() {
        // Wait until both buyer and seller have submitted their first orders
        std::unique_lock<std::mutex> lock(cv_mtx);
        cv.wait(lock, [&buyerReady, &sellerReady]() {
            return buyerReady && sellerReady;
        });

        // Watch the "AAPL" symbol for 3 seconds, and log the results
        client.watchSymbol("AAPL", 3, true);
    };

    // Launch the threads
    std::thread buyer(buyerThread);
    std::thread seller(sellerThread);
    std::thread watcher(watchThread);

    // Join the threads
    buyer.join();
    seller.join();
    watcher.join();

    return 0;
}