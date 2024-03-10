//
//  limit_price.h
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 3/10/24.
//

#ifndef limit_price_h
#define limit_price_h

#include "order.h"

#include <deque>
#include <memory>

using namespace std;

class LimitPrice {
public:
    //const double price;
    unsigned int numOrders;
    unsigned int totalSize;
    LimitPrice* parent;
    LimitPrice* towardsMid;
    LimitPrice* awayFromMid;
    std::deque<std::shared_ptr<Order>> orders; // invariant: never empty. If it empties, delete the LimitPrice.
    //bool hasOrders;                          // can also try to keep a bool in case it speeds up the book.
    LimitPrice();
    LimitPrice(double price_in);
};

#endif /* limit_price_h */
