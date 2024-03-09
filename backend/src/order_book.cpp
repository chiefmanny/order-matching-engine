#include "../include/order_book.h"
#include "../include/order.h"

using namespace std;

std::random_device OrderBook::rd;
std::mt19937 OrderBook::gen(OrderBook::rd()); // Initialize gen with rd
std::uniform_int_distribution<unsigned int> OrderBook::distribution(0, std::numeric_limits<unsigned int>::max());

OrderBook::OrderBook(std::string security_in, unsigned int securityId_in, std::shared_mutex& reader_in, std::shared_mutex& writer_in) : security(security_in), securityId(securityId_in), reader(reader_in), writer(writer_in) {}
LimitPrice::LimitPrice() : price(0.0), numOrders(0), volume(0), parent(nullptr), towardsMid(nullptr), awayFromMid(nullptr) {}
LimitPrice::LimitPrice(double price_in) : price(price_in), numOrders(0), volume(0), parent(nullptr), towardsMid(nullptr), awayFromMid(nullptr) {}

unsigned int OrderBook::addLimitOrder(double price_in, unsigned int size_in, bool buy) {
    if (!price_in || !size_in) return 0; // order_id of 0 implies unsuccessful operation
    unsigned int id = generateOrderId();
    
    std::unique_lock<std::shared_mutex> lock(writer);
    
    std::shared_ptr<Order> addOrder = std::make_shared<Order>(id, size_in, OrderType::Limit, price_in, nullptr, nullptr, nullptr); // pointers uneeded for FIFO
    if (buy) {
        this->askTree[price_in].orders.push_back(addOrder);
        allSellOrders[id] = addOrder;
    }
    else {
        this->bidTree[price_in].orders.push_back(addOrder);
        allBuyOrders[id] = addOrder;
    }
    return id;
}

bool OrderBook::cancelLimitOrder(unsigned int orderId_in) { // could ask for more info from client to reduce server side overhead
    std::unique_lock<std::shared_mutex> lock(writer);
    
    auto buyIt = allBuyOrders.find(orderId_in);
    if (buyIt != allBuyOrders.end()) {
        auto& orderDeque = bidTree[buyIt->second->price].orders;
        orderDeque.erase(std::remove(orderDeque.begin(), orderDeque.end(), buyIt->second), orderDeque.end());
        allBuyOrders.erase(buyIt);
        return true;
    }

    auto sellIt = allSellOrders.find(orderId_in);
    if (sellIt != allSellOrders.end()) {
        auto& orderDeque = askTree[sellIt->second->price].orders;
        orderDeque.erase(std::remove(orderDeque.begin(), orderDeque.end(), sellIt->second), orderDeque.end());
        allSellOrders.erase(sellIt);
        return true;
    }
    return false;
}

unsigned int OrderBook::generateOrderId() {
    std::shared_lock<std::shared_mutex> lock(reader);
    
    unsigned int randomNumber;
    do {
        randomNumber = distribution(gen);
    } while (allBuyOrders.contains(randomNumber) || allSellOrders.contains(randomNumber));

    return randomNumber;
}

LimitPrice* OrderBook::getAsk() {
    std::shared_lock<std::shared_mutex> lock(reader);
    
    return &(askTree.begin()->second);
}

LimitPrice* OrderBook::getBid() {
    std::shared_lock<std::shared_mutex> lock(reader);
    
    return &(bidTree.begin()->second);
}
