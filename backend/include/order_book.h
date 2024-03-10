#ifndef order_book_h
#define order_book_h

#include "order.h"
#include "matching_algorithm.h"

#include <deque>
#include <map>
#include <string>
#include <random>
#include <memory>
#include <thread>
#include <mutex>
#include <shared_mutex>

using namespace std;

struct Order; // Forward declaration

class LimitPrice {
public:
    const double price;
    unsigned int numOrders;
    unsigned int volume;
    LimitPrice* parent;
    LimitPrice* towardsMid;
    LimitPrice* awayFromMid;
    std::deque<std::shared_ptr<Order>> orders; // invariant: never empty. If it empties, delete the LimitPrice.
    //bool hasOrders;                          // can also try to keep a bool in case it speeds up the book.
    LimitPrice();
    LimitPrice(double price_in);
};

class OrderBook {
    const std::string security;
    const unsigned int securityId;
    std::map<double, LimitPrice> askTree;
    std::map<double, LimitPrice, std::greater<double>> bidTree;
    std::unordered_map<unsigned int, std::shared_ptr<Order>> allBuyOrders;
    std::unordered_map<unsigned int, std::shared_ptr<Order>> allSellOrders;
    LimitPrice* ask;
    LimitPrice* bid;
    std::shared_mutex& askMutex;
    std::shared_mutex& bidMutex;
    std::shared_mutex buyOrdersMutex;
    std::shared_mutex sellOrdersMutex;
    
    OrderBook(std::string security_in, unsigned int securityId_in, std::shared_mutex& askMutex_in, std::shared_mutex& bidMutex_in);
    unsigned int addLimitOrder(double price_in, unsigned int size_in, bool buy); // returns an order_id
    bool modifyLimitOrder(double price_in, unsigned int size_in, unsigned int orderId_in);
    bool cancelLimitOrder(unsigned int orderId_in);
    unsigned int generateOrderId();
    void refreshAsk();
    void refreshBid();
    LimitPrice* getAskWithLock();
    LimitPrice* getBidWithLock();
    LimitPrice* getAsk();
    LimitPrice* getBid();

    static std::random_device rd;
    static std::mt19937 gen;
    static std::uniform_int_distribution<unsigned int> distribution;
    
    friend class MatchingAlgorithm;
};

#endif /* order_book_h */
