#include "matching_engine_client.h"
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <iostream>
#include <chrono>
#include <random>
#include <getopt.h>

// Command-Line Usage Example:
// ./mean_reversion_client -s 55.0 -o 150 -w 200 -p 300 -r 120 -a 2.5 -l 0.15

// Explanation of Command-Line Options:
// -s (Seed Price): Set the initial seed price for the bid and ask threads.
// -o (Order Size): Set the average order size (default: 100).
// -w (Watcher Update Interval): Set the watcher update interval in milliseconds (default: 500ms).
// -p (Push Interval): Set the time interval between order placements in milliseconds (default: 500ms).
// -r (Run Time): Set how long the program runs in seconds (default: 60 seconds).
// -a (Aggressiveness Factor): Set the aggressiveness factor for dynamic order sizing when grabbing liquidity (default: 2.0).
// -l (Spread Limit): Set the maximum allowable spread before tightening in dollars (default: 0.20).

// Constants for default values
int AVERAGE_ORDER_SIZE = 100;
double SEED_PRICE = 50.0;
int WATCHER_UPDATE_INTERVAL = 500;  // ms
int PUSH_INTERVAL = 500;            // ms between orders
int RUN_TIME = 60;                  // seconds
double AGGRESSIVENESS_FACTOR = 2.0; // How aggressive orders are when taking liquidity
double SPREAD_LIMIT = 0.20;         // Max allowable spread

// Function to parse command-line arguments
void parseArguments(int argc, char** argv) {
    int option;
    while ((option = getopt(argc, argv, "s:o:w:p:r:a:l:")) != -1) {
        switch (option) {
            case 's':
                SEED_PRICE = atof(optarg);
                break;
            case 'o':
                AVERAGE_ORDER_SIZE = atoi(optarg);
                break;
            case 'w':
                WATCHER_UPDATE_INTERVAL = atoi(optarg);
                break;
            case 'p':
                PUSH_INTERVAL = atoi(optarg);
                break;
            case 'r':
                RUN_TIME = atoi(optarg);
                break;
            case 'a':
                AGGRESSIVENESS_FACTOR = atof(optarg);
                break;
            case 'l':
                SPREAD_LIMIT = atof(optarg);
                break;
            default:
                std::cerr << "Invalid option. Usage: " << argv[0] << " [-s seed_price] [-o order_size] [-w watcher_update_interval] [-p push_interval] [-r run_time] [-a aggressiveness_factor] [-l spread_limit]\n";
                exit(EXIT_FAILURE);
        }
    }
}

// Function to determine whether it's time to push the price up or down based on current seconds
bool shouldPushPriceUp() {
    auto now = std::chrono::system_clock::now();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() % 60;
    return (seconds >= 0 && seconds < 15) || (seconds >= 30 && seconds < 45);
}

// Function to generate a random price adjustment in dollar amounts (more dramatic)
double randomPriceAdjustment(double basePrice, double minJump = 1.0, double maxJump = 5.0) {
    static std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> dist(minJump, maxJump);  // Price movement range: $1 to $5
    return basePrice + dist(rng);
}

// Function to calculate dynamic order sizes (more aggressive or passive depending on situation)
int calculateOrderSize(bool isAggressive) {
    static std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> passiveFactor(0.5, 0.8);  // Factor for passive orders
    std::uniform_real_distribution<double> aggressiveFactor(1.5 * AGGRESSIVENESS_FACTOR, 3.0 * AGGRESSIVENESS_FACTOR);  // Factor for aggressive orders

    double factor = isAggressive ? aggressiveFactor(rng) : passiveFactor(rng);
    return static_cast<int>(AVERAGE_ORDER_SIZE * factor);  // Dynamically calculate the order size
}

// Function to check if the spread between the best bid and ask is greater than the defined spread limit
bool isSpreadTooWide(double bestBid, double bestAsk) {
    return (bestAsk - bestBid) > SPREAD_LIMIT;
}

// Function for the bid-side trading thread (buyers stepping up)
void bidSideTrading(MatchingEngineClient& client, std::mutex& dataMutex, std::condition_variable& cv,
                    std::pair<double, double>& bestBidAsk, bool& dataAvailable, double seedPrice) {
    std::atomic<bool> bidPlaced(false);

    // Thread to watch the symbol (bid side)
    std::thread bidWatcher([&]() {
        std::cout << "\033[34m[Bid Thread] Starting bid-side watcher...\033[0m\n";
        client.watchSymbol_data("AAPL", RUN_TIME, dataMutex, cv, bestBidAsk, dataAvailable);  // Watch for bid/ask changes
    });

    // Thread for placing and canceling orders on the bid side
    std::thread bidTrader([&]() {
        std::cout << "\033[34m[Bid Thread] Populating order book with initial bid orders based on seed value...\033[0m\n";
        std::unique_lock<std::mutex> lock(dataMutex);

        // Populate the order book with initial bid orders based on the seed price
        for (int i = 0; i < 10; ++i) {
            double price = seedPrice - i * 1.0;  // Bids starting below the seed price
            int orderSize = calculateOrderSize(false);  // Passive order size for initial orders
            client.addOrder("AAPL", price, orderSize, true);  // Add initial bid orders
            std::cout << "\033[33m[Bid Thread] Added initial bid order: Price = " << price << " Size = " << orderSize << "\033[0m\n";
        }

        // Main loop: continuous bid adjustment based on watcher data and clock
        auto startTime = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startTime).count() < RUN_TIME) {
            // Wait for the watcher to update the best bid/ask price
            std::cout << "\033[34m[Bid Thread] Waiting for watcher to update best bid price...\033[0m\n";
            while (!dataAvailable) {
                cv.wait(lock);
            }
            std::cout << "\033[34m[Bid Thread] Received watcher data: Best Bid = " << bestBidAsk.first 
                      << ", Best Ask = " << bestBidAsk.second << "\033[0m\n";

            // Check if the spread is too wide, and place orders to tighten it
            if (isSpreadTooWide(bestBidAsk.first, bestBidAsk.second)) {
                // Aggressive order to tighten the spread
                double priceToTighten = bestBidAsk.second - 0.10;  // Bid 10 cents below the ask to tighten spread
                int orderSize = calculateOrderSize(true);  // Aggressive order size
                client.addOrder("AAPL", priceToTighten, orderSize, true);
                std::cout << "\033[35m[Bid Thread] Placed bid to tighten spread: Price = " << priceToTighten 
                          << " Size = " << orderSize << "\033[0m\n";
            } else {
                // Buyers stepping up: to move the market, place orders **above the best ASK** to take liquidity
                if (shouldPushPriceUp()) {
                    // Aggressive order to move price upwards
                    double priceToBid = randomPriceAdjustment(bestBidAsk.second + 1.0, 1.0, 5.0);  // Bid above best ask
                    int orderSize = calculateOrderSize(true);  // Aggressive order size
                    client.addOrder("AAPL", priceToBid, orderSize, true);  // Place a bid above best ask to move price up
                    std::cout << "\033[31m[Bid Thread] Placed aggressive bid above ask to push price up: Price = " 
                              << priceToBid << " Size = " << orderSize << "\033[0m\n";
                } else {
                    // Passive order to push price downwards
                    double priceToBid = randomPriceAdjustment(bestBidAsk.first - 1.0, -5.0, -1.0);  // Stay close to the bid
                    int orderSize = calculateOrderSize(false);  // Passive order size
                    client.addOrder("AAPL", priceToBid, orderSize, true);  // Adjust bid downward
                    std::cout << "\033[31m[Bid Thread] Adjusted bid downward: Price = " << priceToBid 
                              << " Size = " << orderSize << "\033[0m\n";
                }
            }

            // Sleep briefly to prevent too rapid execution
            cv.wait_for(lock, std::chrono::milliseconds(PUSH_INTERVAL));  // Adjust frequency
        }
    });

    bidWatcher.join();
    bidTrader.join();
}

// Function for the ask-side trading thread (sellers stepping down)
void askSideTrading(MatchingEngineClient& client, std::mutex& dataMutex, std::condition_variable& cv,
                    std::pair<double, double>& bestBidAsk, bool& dataAvailable, double seedPrice) {
    std::atomic<bool> askPlaced(false);

    // Thread to watch the symbol (ask side)
    std::thread askWatcher([&]() {
        std::cout << "\033[32m[Ask Thread] Starting ask-side watcher...\033[0m\n";
        client.watchSymbol_data("AAPL", RUN_TIME, dataMutex, cv, bestBidAsk, dataAvailable);  // Watch for bid/ask changes
    });

    // Thread for placing and canceling orders on the ask side
    std::thread askTrader([&]() {
        std::cout << "\033[32m[Ask Thread] Populating order book with initial ask orders based on seed value...\033[0m\n";
        std::unique_lock<std::mutex> lock(dataMutex);

        // Populate the order book with initial ask orders based on the seed price
        for (int i = 0; i < 10; ++i) {
            double price = seedPrice + i * 1.0;  // Asks starting above the seed price
            int orderSize = calculateOrderSize(false);  // Passive order size for initial orders
            client.addOrder("AAPL", price, orderSize, false);  // Add initial ask orders
            std::cout << "\033[33m[Ask Thread] Added initial ask order: Price = " << price << " Size = " << orderSize << "\033[0m\n";
        }

        // Main loop: continuous ask adjustment based on watcher data and clock
        auto startTime = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startTime).count() < RUN_TIME) {
            // Wait for the watcher to update the best ask price
            std::cout << "\033[32m[Ask Thread] Waiting for watcher to update best ask price...\033[0m\n";
            while (!dataAvailable) {
                cv.wait(lock);
            }
            std::cout << "\033[32m[Ask Thread] Received watcher data: Best Bid = " << bestBidAsk.first 
                      << ", Best Ask = " << bestBidAsk.second << "\033[0m\n";

            // Check if the spread is too wide, and place orders to tighten it
            if (isSpreadTooWide(bestBidAsk.first, bestBidAsk.second)) {
                // Aggressive order to tighten the spread
                double priceToTighten = bestBidAsk.first + 0.10;  // Ask 10 cents above the bid to tighten spread
                int orderSize = calculateOrderSize(true);  // Aggressive order size
                client.addOrder("AAPL", priceToTighten, orderSize, false);
                std::cout << "\033[35m[Ask Thread] Placed ask to tighten spread: Price = " << priceToTighten 
                          << " Size = " << orderSize << "\033[0m\n";
            } else {
                // Sellers stepping down: to move the market, place orders **below the best BID** to take liquidity
                if (!shouldPushPriceUp()) {
                    // Aggressive order to move price downwards
                    double priceToAsk = randomPriceAdjustment(bestBidAsk.first - 1.0, -5.0, -1.0);  // Ask below best bid
                    int orderSize = calculateOrderSize(true);  // Aggressive order size
                    client.addOrder("AAPL", priceToAsk, orderSize, false);  // Place an ask below best bid to move price down
                    std::cout << "\033[31m[Ask Thread] Placed aggressive ask below bid to push price down: Price = " 
                              << priceToAsk << " Size = " << orderSize << "\033[0m\n";
                } else {
                    // Passive order to push price upwards
                    double priceToAsk = randomPriceAdjustment(bestBidAsk.second + 1.0, 1.0, 5.0);  // Stay close to the ask
                    int orderSize = calculateOrderSize(false);  // Passive order size
                    client.addOrder("AAPL", priceToAsk, orderSize, false);  // Adjust ask upward
                    std::cout << "\033[31m[Ask Thread] Adjusted ask upward: Price = " << priceToAsk 
                              << " Size = " << orderSize << "\033[0m\n";
                }
            }

            // Sleep briefly to prevent too rapid execution
            cv.wait_for(lock, std::chrono::milliseconds(PUSH_INTERVAL));  // Adjust frequency
        }
    });

    askWatcher.join();
    askTrader.join();
}

// Main function
int main(int argc, char** argv) {
    MatchingEngineClient bidClient("localhost:50051");
    MatchingEngineClient askClient("localhost:50051");
    MatchingEngineClient printClient("localhost:50051");

    // Parse command-line options
    parseArguments(argc, argv);

    // Independent shared data for bid and ask prices
    std::pair<double, double> bestBidAskBid = {0.0, 0.0};  // {bestBid, bestAsk} for the bid thread
    std::pair<double, double> bestBidAskAsk = {0.0, 0.0};  // {bestBid, bestAsk} for the ask thread

    // Synchronization
    std::mutex bidDataMutex;
    std::mutex askDataMutex;
    std::condition_variable bidCv;
    std::condition_variable askCv;
    bool bidDataAvailable = false;
    bool askDataAvailable = false;

    // Spawn the two trading threads with independent watchers and data, based on seed price
    std::thread bidThread(bidSideTrading, std::ref(bidClient), std::ref(bidDataMutex), std::ref(bidCv),
                          std::ref(bestBidAskBid), std::ref(bidDataAvailable), SEED_PRICE);

    std::thread askThread(askSideTrading, std::ref(askClient), std::ref(askDataMutex), std::ref(askCv),
                          std::ref(bestBidAskAsk), std::ref(askDataAvailable), SEED_PRICE);

    // Start the watchSymbol_print thread to log the data to a CSV
    std::thread printWatcher([&]() {
        std::cout << "\033[36m[Main Thread] Starting print watcher...\033[0m\n";
        printClient.watchSymbol_print("AAPL", RUN_TIME, true);  // Watch and log data to CSV
        std::cout << "\033[36m[Main Thread] Finished logging data to CSV.\033[0m\n";
    });

    // Join the threads after execution
    bidThread.join();
    askThread.join();
    printWatcher.join();  // Ensure the printWatcher finishes logging before terminating

    std::cout << "\033[36m[Main Thread] All threads finished execution.\033[0m\n";
    return 0;
}
