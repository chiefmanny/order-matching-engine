#ifndef order_h
#define order_h

#include <cstdint>

enum OrderType {
    Limit,
    Market
};

struct OrderInternal {
    unsigned int orderId;
    unsigned int size;
    OrderType type;
    double price;
    OrderInternal* up;
    OrderInternal* down;

    // Monotonic arrival sequence, assigned by OrderBook::addLimitOrder.  When two
    // orders match, the one with the LOWER seq arrived first and is therefore the
    // resting order (the maker); the other is the aggressor (the taker).  This is
    // what lets the engine report a standard price-time-priority trade price and
    // an unambiguous aggressor side.
    uint64_t seq;

    OrderInternal(unsigned int orderId_in,
          unsigned int size_in,
          OrderType type = OrderType::Limit,
          double price_in = 0.0,
          OrderInternal* up_in = nullptr,
          OrderInternal* down_in = nullptr,
          uint64_t seq_in = 0);
};

#endif /* order_h */
