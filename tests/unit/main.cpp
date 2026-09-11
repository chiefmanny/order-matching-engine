//
//  main.cpp
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 2/29/24.
//
//  Legacy multithreaded order-book stress tests.  These spin up many threads,
//  run the matching algorithm, and produce heavy console output — they are slow
//  stress/integration checks, not fast unit tests.  The fast participant unit
//  tests live in the separate participant_tests target.
//

#include <iostream>
#include <thread>
#include <cassert>
#include <set>

#include "unit_tests.h"

using namespace std;

int main(int argc, const char * argv[]) {
    unsigned int n = std::thread::hardware_concurrency();
    std::cout << n << " concurrent threads are supported.\n";

    std::cout << "Running tests " << std::endl;
    orderbook_basic_test_1();
    orderbook_multithread_test_1();
    orderbook_multithread_test_2();
    orderbook_multithread_test_3();
    randomized_orderbook_test();
    skewed_orderbook_test(50.0, 20);
    randomized_orderbook_test_buy_trend();
    massive_orderbook_test_price_movement();

    std::cout << "All tests passed " << std::endl;

    return 0;
}
