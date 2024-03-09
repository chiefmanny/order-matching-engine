//
//  main.cpp
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 2/29/24.
//

#include <iostream>
#include "backend/include/order_book.h"
#include "backend/include/order.h"
#include <thread>

OrderBook book();

int main(int argc, const char * argv[]) {
    // insert code here...
    unsigned int n = std::thread::hardware_concurrency();
    std::cout << n << " concurrent threads are supported.\n";
    std::cout << "Hello, World!\n";
    return 0;
}
