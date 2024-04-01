#include "order_book.h"

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
    if (!price_in || !size_in) return 0; // order_id of 0 implies unsuccessful operation
    unsigned int id = generateOrderId();
    
    std::shared_ptr<OrderInternal> addOrder = std::make_shared<OrderInternal>(id, size_in, OrderType::Limit, price_in, nullptr, nullptr); // pointers uneeded for FIFO
    if (buy) {
        std::unique_lock<std::shared_mutex> lockTree(bidMutex);
        this->bidTree[price_in].orders.push_back(addOrder);
        this->bidTree[price_in].totalSize += addOrder->size;
        std::unique_lock<std::shared_mutex> lockHash(buyOrdersMutex);
        allBuyOrders[id] = addOrder;
    } else {
        std::unique_lock<std::shared_mutex> lockTree(askMutex);
        this->askTree[price_in].orders.push_back(addOrder);
        this->askTree[price_in].totalSize += addOrder->size;
        std::unique_lock<std::shared_mutex> lockHash(sellOrdersMutex);
        allSellOrders[id] = addOrder;
    }
    return id;
}

bool OrderBook::cancelLimitOrder(unsigned int orderId_in) { // could ask for more info from client to reduce server side overhead
    {
        std::unique_lock<std::shared_mutex> lockTree(bidMutex);
        std::unique_lock<std::shared_mutex> lockHash(buyOrdersMutex);
        auto buyIt = allBuyOrders.find(orderId_in);
        if (buyIt != allBuyOrders.end()) {
            const double price = buyIt->second->price;
            auto& limit = bidTree[buyIt->second->price];
            limit.orders.erase(std::remove(limit.orders.begin(), limit.orders.end(), buyIt->second), limit.orders.end());
            limit.totalSize -= buyIt->second->size;
            allBuyOrders.erase(buyIt);
            if (limit.orders.empty()) {
                bidTree.erase(price);
                bid = &(bidTree.begin()->second); // refreshBid
            }
            return true;
        }
    }
    {
        std::unique_lock<std::shared_mutex> lockTree(askMutex);
        std::unique_lock<std::shared_mutex> lockHash(sellOrdersMutex);
        auto sellIt = allSellOrders.find(orderId_in);
        if (sellIt != allSellOrders.end()) {
            const double price = sellIt->second->price;
            auto& limit = askTree[sellIt->second->price];
            limit.orders.erase(std::remove(limit.orders.begin(), limit.orders.end(), sellIt->second), limit.orders.end());
            limit.totalSize -= sellIt->second->size;
            allSellOrders.erase(sellIt);
            if (limit.orders.empty()) {
                askTree.erase(price);
                ask = &(askTree.begin()->second); // refreshAsk
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

void OrderBook::refreshAsk() {
    std::shared_lock<std::shared_mutex> lockSellHash(sellOrdersMutex);
    ask = &(askTree.begin()->second);
}

void OrderBook::refreshBid() {
    std::shared_lock<std::shared_mutex> lockBuyHash(buyOrdersMutex);
    bid = &(bidTree.begin()->second);
}

bool OrderBook::askEmpty() {
    return askTree.empty();
}

bool OrderBook::bidEmpty() {
    return bidTree.empty();
}
