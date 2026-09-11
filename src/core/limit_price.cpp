//
//  limit_price.cpp
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 3/10/24.
//

#include <stdio.h>

#include "limit_price.h"

// numOrders and totalSize MUST be zero-initialized here.  LimitPrice is created
// implicitly by `bidTree[price]` / `askTree[price]`, which value-initializes via the
// default constructor; leaving totalSize uninitialized made every GetLevel2 depth
// number undefined.
LimitPrice::LimitPrice() : /*price(0.0),*/ numOrders(0), totalSize(0), parent(nullptr), towardsMid(nullptr), awayFromMid(nullptr) {}
//LimitPrice::LimitPrice(double price_in) : price(price_in), numOrders(0), totalSize(0), parent(nullptr), towardsMid(nullptr), awayFromMid(nullptr) {}
