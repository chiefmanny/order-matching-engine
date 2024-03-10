#ifndef matching_algorithm_h
#define matching_algorithm_h

#include "order.h"
#include "order_book.h"
#include <mutex>
#include <shared_mutex>
#include <thread>

using namespace std;

enum MatchingAlgorithmType {
    Simple
};

class MatchingAlgorithm {
    
    MatchingAlgorithm(OrderBook& orderbook_in, std::shared_mutex& askMutex_in, std::shared_mutex& bidMutex_in);
    void run();
    
    std::shared_mutex& askMutex;
    std::shared_mutex& bidMutex;
    OrderBook& orderbook;
};

#endif /* matching_algorithm_h */
