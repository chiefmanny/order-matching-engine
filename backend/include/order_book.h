#ifndef order_book_h
#define order_book_h

#include "../include/limit_price.h"

#include <deque>
#include <map>
#include <string>
#include <random>
#include <memory>
#include <thread>
#include <mutex>
#include <shared_mutex>

using namespace std;

class OrderBook {
public:
    const std::string security;
    const unsigned int securityId;
    unsigned int volume;
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
    const double getAsk();
    const double getBid();
    bool askEmpty();
    bool bidEmpty();

    static std::random_device rd;
    static std::mt19937 gen;
    static std::uniform_int_distribution<unsigned int> distribution;
    
    friend class MatchingAlgorithm;
};

#endif /* order_book_h */
