#ifndef order_h
#define order_h

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
    
    Order(unsigned int orderId_in,
          unsigned int size_in,
          OrderType type = OrderType::Limit,
          double price_in = 0.0,
          Order* up_in = nullptr,
          Order* down_in = nullptr);
};

#endif /* order_h */
