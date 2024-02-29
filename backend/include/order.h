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
};

#endif /* order_h */
