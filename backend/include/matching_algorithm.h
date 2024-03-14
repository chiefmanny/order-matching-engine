#ifndef matching_algorithm_h
#define matching_algorithm_h

#include <mutex>
#include <shared_mutex>
#include <thread>
#include "../include/order_book.h"

class OrderBook;

using namespace std;

enum MatchingAlgorithmType {
    Simple
};

class MatchingAlgorithm {
private:
    std::shared_mutex& askMutex;
    std::shared_mutex& bidMutex;
    OrderBook& orderbook;
    
public:
    MatchingAlgorithm(OrderBook& orderbook_in, std::shared_mutex& askMutex_in, std::shared_mutex& bidMutex_in);
    void run(bool testFlag);
    
};

#endif /* matching_algorithm_h */
