#ifndef order_h
#define order_h

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
    
    OrderInternal(unsigned int orderId_in,
          unsigned int size_in,
          OrderType type = OrderType::Limit,
          double price_in = 0.0,
          OrderInternal* up_in = nullptr,
          OrderInternal* down_in = nullptr);
};

#endif /* order_h */
