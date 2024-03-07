#ifndef order_h
#define order_h

#include "order_book.h"

using namespace std;

struct LimitPrice; // Forward declaration

enum OrderType {
    Limit,
    Market
};

struct Order {
    unsigned int orderId;
    unsigned int size;
    OrderType type;
    double price;
    Order* up;
    Order* down;
    LimitPrice* limit;
    
    Order(unsigned int orderId_in,
          unsigned int size_in,
          OrderType type = OrderType::Limit,
          double price_in = 0.0,
          Order* up_in = nullptr,
          Order* down_in = nullptr,
          LimitPrice* limit_in = nullptr);
};

#endif /* order_h */
