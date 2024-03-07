#ifndef order_book_h
#define order_book_h

#include "order.h"

#include <deque>
#include <map>
#include <string>
#include <random>
#include <memory>

using namespace std;

struct Order; // Forward declaration

struct LimitPrice {
    const double price;
    unsigned int numOrders;
    unsigned int volume;
    LimitPrice* parent;
    LimitPrice* towardsMid;
    LimitPrice* awayFromMid;
    std::deque<std::shared_ptr<Order>> orders;
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
    
    OrderBook(std::string security_in, unsigned int securityId_in);
    unsigned int addLimitOrder(double price_in, unsigned int size_in, bool buy); // returns an order_id
    bool modifyLimitOrder(double price_in, unsigned int size_in, unsigned int orderId_in);
    bool cancelLimitOrder(unsigned int orderId_in);
    unsigned int generateOrderId();

    static std::random_device rd;
    static std::mt19937 gen;
    static std::uniform_int_distribution<unsigned int> distribution;
    
};

#endif /* order_book_h */
