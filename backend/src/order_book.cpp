#include "../include/order_book.h"
#include "../include/order.h"

using namespace std;

std::random_device OrderBook::rd;
std::mt19937 OrderBook::gen(OrderBook::rd()); // Initialize gen with rd
std::uniform_int_distribution<unsigned int> OrderBook::distribution(0, std::numeric_limits<unsigned int>::max());

OrderBook::OrderBook(std::string security_in, unsigned int securityId_in, std::shared_mutex& askMutex_in, std::shared_mutex& bidMutex_in) : security(security_in), securityId(securityId_in), askMutex(askMutex_in), bidMutex(bidMutex_in) {}
LimitPrice::LimitPrice() : price(0.0), numOrders(0), volume(0), parent(nullptr), towardsMid(nullptr), awayFromMid(nullptr) {}
LimitPrice::LimitPrice(double price_in) : price(price_in), numOrders(0), volume(0), parent(nullptr), towardsMid(nullptr), awayFromMid(nullptr) {}

unsigned int OrderBook::addLimitOrder(double price_in, unsigned int size_in, bool buy) {
    if (!price_in || !size_in) return 0; // order_id of 0 implies unsuccessful operation
    unsigned int id = generateOrderId();
    
    std::shared_ptr<Order> addOrder = std::make_shared<Order>(id, size_in, OrderType::Limit, price_in, nullptr, nullptr, nullptr); // pointers uneeded for FIFO
    if (buy) {
        std::unique_lock<std::shared_mutex> lockTree(bidMutex);
        this->askTree[price_in].orders.push_back(addOrder);
        std::unique_lock<std::shared_mutex> lockHash(buyOrdersMutex);
        allSellOrders[id] = addOrder;
    } else {
        std::unique_lock<std::shared_mutex> lockTree(askMutex);
        this->bidTree[price_in].orders.push_back(addOrder);
        std::unique_lock<std::shared_mutex> lockHash(sellOrdersMutex);
        allBuyOrders[id] = addOrder;
    }
    return id;
}

bool OrderBook::cancelLimitOrder(unsigned int orderId_in) { // could ask for more info from client to reduce server side overhead
    {
        std::unique_lock<std::shared_mutex> lockTree(bidMutex);
        std::unique_lock<std::shared_mutex> lockHash(buyOrdersMutex);
        auto buyIt = allBuyOrders.find(orderId_in);
        if (buyIt != allBuyOrders.end()) {
            auto& orderDeque = bidTree[buyIt->second->price].orders;
            orderDeque.erase(std::remove(orderDeque.begin(), orderDeque.end(), buyIt->second), orderDeque.end());
            allBuyOrders.erase(buyIt);
            if (orderDeque.empty()) {
                bidTree.erase(buyIt->second->price);
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
            auto& orderDeque = askTree[sellIt->second->price].orders;
            orderDeque.erase(std::remove(orderDeque.begin(), orderDeque.end(), sellIt->second), orderDeque.end());
            allSellOrders.erase(sellIt);
            if (orderDeque.empty()) {
                askTree.erase(sellIt->second->price);
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
    } while (allBuyOrders.contains(randomNumber) || allSellOrders.contains(randomNumber));

    return randomNumber;
}

LimitPrice* OrderBook::getAsk() { // Need to change this to check if there are actual orders at the level too. Otherwise, the bid and ask best prices will evaluate to the best seen prices, even if no orders are present at those prices
    std::shared_lock<std::shared_mutex> lockSellHash(sellOrdersMutex);
    
    return &(askTree.begin()->second);
}

LimitPrice* OrderBook::getBid() {
    std::shared_lock<std::shared_mutex> lockBuyHash(buyOrdersMutex);
    
    return &(bidTree.begin()->second);
}

void OrderBook::refreshAsk() {
    std::shared_lock<std::shared_mutex> lockSellHash(sellOrdersMutex);
    ask = &(askTree.begin()->second);
}

void OrderBook::refreshBid() {
    std::shared_lock<std::shared_mutex> lockBuyHash(buyOrdersMutex);
    bid = &(bidTree.begin()->second);
}
