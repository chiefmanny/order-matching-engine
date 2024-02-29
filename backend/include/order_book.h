#ifndef order_book_h
#define order_book_h

#include "order.h"

#include <deque>
#include <map>
#include <string>

using namespace std;

struct LimitPrice {
    const double price;
    unsigned int numOrders;
    unsigned int volume;
    LimitPrice* parent;
    LimitPrice* towardsMid;
    LimitPrice* awayFromMid;
    std::deque<Order>* orders;
};

struct BidFunctor {
    bool operator()(const LimitPrice& lhs, const LimitPrice& rhs) const {
        return lhs.price < rhs.price;
    }
};

struct AskFunctor {
    bool operator()(const LimitPrice& lhs, const LimitPrice& rhs) const {
        return lhs.price > rhs.price;
    }
};

class OrderBook {
    const std::string security;
    const unsigned int securityId;
    std::map<LimitPrice, BidFunctor> askTree;
    std::map<LimitPrice, AskFunctor> bidTree;
    LimitPrice* ask;
    LimitPrice* bid;
    
    OrderBook(std::string security_in, unsigned int securityId_in);
    bool addLimitOrder(double price_in, double size_in, unsigned int orderId_in, bool buy);
    bool modifyLimitOrder(double price_in, double size_in, unsigned int orderId_in);
    bool cancelLimitOrder(unsigned int orderId_in);
};

#endif /* order_book_h */
