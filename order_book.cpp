#include "order_book.h"
#include <iostream>

using namespace std;

std::random_device OrderBook::rd;
std::mt19937 OrderBook::gen(OrderBook::rd()); // Initialize gen with rd
std::uniform_int_distribution<unsigned int> OrderBook::distribution(0, std::numeric_limits<unsigned int>::max());

OrderBook::OrderBook(std::string security_in, unsigned int securityId_in, std::shared_mutex& askMutex_in, std::shared_mutex& bidMutex_in) : security(security_in), securityId(securityId_in), askMutex(askMutex_in), bidMutex(bidMutex_in) {
    ask = nullptr;
    bid = nullptr;
    volume = 0;
}

unsigned int OrderBook::addLimitOrder(double price_in, unsigned int size_in, bool buy) {
    if (price_in <= 0.0 || size_in == 0) return 0; // order_id of 0 implies unsuccessful operation
    unsigned int id = generateOrderId();

    std::shared_ptr<OrderInternal> addOrder = std::make_shared<OrderInternal>(id, size_in, OrderType::Limit, price_in, nullptr, nullptr); // pointers unneeded for FIFO

    if (buy) {
        std::scoped_lock lockTreeAndHash(bidMutex, buyOrdersMutex);
        this->bidTree[price_in].orders.push_back(addOrder);
        this->bidTree[price_in].totalSize += addOrder->size;
        allBuyOrders[id] = addOrder;
    } else {
        std::scoped_lock lockTreeAndHash(askMutex, sellOrdersMutex);
        this->askTree[price_in].orders.push_back(addOrder);
        this->askTree[price_in].totalSize += addOrder->size;
        allSellOrders[id] = addOrder;
    }
    return id;
}


bool OrderBook::cancelLimitOrder(unsigned int orderId_in) { // could ask for more info from client to reduce server side overhead
    {
        std::scoped_lock lockTreeAndHash(bidMutex, buyOrdersMutex);
        auto buyIt = allBuyOrders.find(orderId_in);
        if (buyIt != allBuyOrders.end()) {
            const double price = buyIt->second->price;
            auto& limit = bidTree[buyIt->second->price];
            limit.orders.erase(std::remove(limit.orders.begin(), limit.orders.end(), buyIt->second), limit.orders.end());
            limit.totalSize -= buyIt->second->size;
            allBuyOrders.erase(buyIt);
            if (limit.orders.empty()) {
                std::cout << "Thread ID" << std::this_thread::get_id() << " Old Bid: " << price << std::endl;
                bidTree.erase(price);
                refreshBidPointer(); // refreshBid
                std::cout << "Thread ID" << std::this_thread::get_id() << " New Bid: " << (bidTree.begin()->first) << std::endl;
            }
            return true;
        }
    }
    {
        std::scoped_lock lockTreeAndHash(askMutex, sellOrdersMutex);
        auto sellIt = allSellOrders.find(orderId_in);
        if (sellIt != allSellOrders.end()) {
            const double price = sellIt->second->price;
            auto& limit = askTree[sellIt->second->price];
            limit.orders.erase(std::remove(limit.orders.begin(), limit.orders.end(), sellIt->second), limit.orders.end());
            limit.totalSize -= sellIt->second->size;
            allSellOrders.erase(sellIt);
            if (limit.orders.empty()) {
                std::cout << "Thread ID" << std::this_thread::get_id() << " Old Ask: " << price << std::endl;
                askTree.erase(price);
                refreshAskPointer(); // refreshAsk
                std::cout << "Thread ID" << std::this_thread::get_id() << " New Ask: " << (askTree.begin()->first) << std::endl;
            }
            return true;
        }
    }
    return false;
}

unsigned int OrderBook::generateOrderId() {
    std::shared_lock<std::shared_mutex> lockBuyHash(buyOrdersMutex);
    std::shared_lock<std::shared_mutex> lockSellHash(sellOrdersMutex);
    unsigned int randomNumber;
    do {
        randomNumber = distribution(gen);
    } while (allBuyOrders.find(randomNumber) != allBuyOrders.end() || allSellOrders.find(randomNumber) != allSellOrders.end());

    return randomNumber;
}

const double OrderBook::getAsk() {
    return askTree.begin()->first;
}

const double OrderBook::getBid() {
    return bidTree.begin()->first;
}

const double OrderBook::getAskWithLock() {
    std::shared_lock<std::shared_mutex> lockAsks(askMutex);
    return askTree.begin()->first;
}

const double OrderBook::getBidWithLock() {
    std::shared_lock<std::shared_mutex> lockBids(bidMutex);
    return bidTree.begin()->first;
}

const std::pair<std::pair<double, unsigned int>, std::pair<double, unsigned int>> OrderBook::getBidSizeAskSizeWithLock() {
    std::shared_lock<std::shared_mutex> lockAsks(askMutex);
    std::shared_lock<std::shared_mutex> lockBids(bidMutex);

    if (askTree.empty() && bidTree.empty()) {
        return {{0.0, 0}, {0.0, 0}};  // Handle empty order books.
    }

    auto askIt = askTree.empty() ? askTree.end() : askTree.begin();
    auto bidIt = bidTree.empty() ? bidTree.end() : bidTree.begin();

    std::pair<double, unsigned int> askInfo = {0.0, 0};
    std::pair<double, unsigned int> bidInfo = {0.0, 0};

    if (askIt != askTree.end()) {
        askInfo.first = askIt->first;
        askInfo.second = askIt->second.orders.empty() ? 0 : static_cast<unsigned int>(askIt->second.orders.front()->size);
    }

    if (bidIt != bidTree.end()) {
        bidInfo.first = bidIt->first;
        bidInfo.second = bidIt->second.orders.empty() ? 0 : static_cast<unsigned int>(bidIt->second.orders.front()->size);
    }

    return {askInfo, bidInfo};
}

double OrderBook::refreshAskPointer() {
    if (!askEmpty()) {
        ask = &(askTree.begin()->second);
        return askTree.begin()->first;
    }
    else {
        ask = nullptr;
        return 0;
    }
}

double OrderBook::refreshBidPointer() {
    if (!bidEmpty()) {
        bid = &(bidTree.begin()->second);
        return bidTree.begin()->first;
    }
    else {
        bid = nullptr;
        return 0;
    }
}

bool OrderBook::askEmpty() {
    return askTree.empty();
}

bool OrderBook::bidEmpty() {
    return bidTree.empty();
}
