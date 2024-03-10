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

#include "backend/include/order_book.h"

using namespace std;

void test2() {
    std::shared_mutex ask;
    std::shared_mutex bid;
    OrderBook book("GME", 1234567890, ask, bid);
    
    std::vector<unsigned int> orderIds;
    
    orderIds.push_back(book.addLimitOrder(10.5, 10, true));
    orderIds.push_back(book.addLimitOrder(11.5, 10, true));
    orderIds.push_back(book.addLimitOrder(12.5, 10, true)); // bids
    orderIds.push_back(book.addLimitOrder(13.5, 10, true));
    orderIds.push_back(book.addLimitOrder(14.5, 10, true));
    
    orderIds.push_back(book.addLimitOrder(14.5, 10, false));
    orderIds.push_back(book.addLimitOrder(15.5, 10, false));
    orderIds.push_back(book.addLimitOrder(16.5, 10, false)); // asks
    orderIds.push_back(book.addLimitOrder(17.5, 10, false));
    orderIds.push_back(book.addLimitOrder(18.5, 10, false));
    
    assert(book.allBuyOrders.size() == 5);
    assert(book.allSellOrders.size() == 5);
    std::set<unsigned int> uniqueIds(orderIds.begin(), orderIds.end());
    assert(uniqueIds.size() == 10);
    
    assert(book.getAsk() == 14.5);
    assert(book.getBid() == 14.5);
    
    assert(book.askTree[14.5].totalSize == 10);
    assert(book.askTree[14.5].totalSize == 10);
    
    book.cancelLimitOrder(orderIds[5]);
    book.cancelLimitOrder(orderIds[4]);
    
    assert(!book.askTree.contains(14.5));
    assert(!book.bidTree.contains(14.5));
    
    assert(book.getAsk() == 15.5);
    assert(book.getBid() == 13.5);
    
    orderIds.push_back(book.addLimitOrder(15.5, 15, false));
    orderIds.push_back(book.addLimitOrder(15.5, 25, false));
    orderIds.push_back(book.addLimitOrder(15.5, 50, false));
    orderIds.push_back(book.addLimitOrder(15.5, 150, false));
    
    assert(book.volume == 0);
    
    assert(book.askTree[15.5].totalSize == 250);
}

int main(int argc, const char * argv[]) {
    // insert code here...
    unsigned int n = std::thread::hardware_concurrency();
    std::cout << n << " concurrent threads are supported.\n";
    
    test2();
    
    return 0;
}
