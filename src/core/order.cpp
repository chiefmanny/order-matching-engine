#include "order.h"

OrderInternal::OrderInternal(unsigned int orderId_in,
             unsigned int size_in,
             OrderType type,
             double price_in,
             OrderInternal* up_in,
             OrderInternal* down_in,
             uint64_t seq_in) : orderId(orderId_in), size(size_in), type(type), price(price_in), up(up_in), down(down_in), seq(seq_in) {}
