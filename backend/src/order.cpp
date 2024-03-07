#include "../include/order.h"

Order::Order(unsigned int orderId_in,
             unsigned int size_in,
             OrderType type,
             double price_in,
             Order* up_in,
             Order* down_in,
             LimitPrice* limit_in) : orderId(orderId_in), size(size_in), type(type), price(price_in), up(up_in), down(down_in), limit(limit_in) {}
