//
//  unit_tests.h
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 3/10/24.
//

#ifndef unit_tests_h
#define unit_tests_h

void orderbook_basic_test_1();

void orderbook_multithread_test_1();

void orderbook_multithread_test_2();

void orderbook_multithread_test_3();

void randomized_orderbook_test();

void skewed_orderbook_test(double midPrice, unsigned int maxSpread);

void randomized_orderbook_test_buy_trend();

void massive_orderbook_test_price_movement();

#endif /* unit_tests_h */
