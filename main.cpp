//
//  main.cpp
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 2/29/24.
//

#include <iostream>
#include <thread>
#include <cassert>
#include <set>

#include "unit_tests.h"
#include "order_book.h"

using namespace std;

int main(int argc, const char * argv[]) {
    // insert code here...
    unsigned int n = std::thread::hardware_concurrency();
    std::cout << n << " concurrent threads are supported.\n";
    
    std::cout << "Running tests " << std::endl;
    //orderbook_basic_test_1();
    //orderbook_multithread_test_1();
    //orderbook_multithread_test_2();
    //orderbook_multithread_test_3();
    //randomized_orderbook_test();
    //skewed_orderbook_test(50.0, 20);
    //massive_orderbook_test_price_movement();
    //massive_orderbook_test_price_jump();
    std::cout << "All tests passed " << std::endl;
    
    return 0;
}
