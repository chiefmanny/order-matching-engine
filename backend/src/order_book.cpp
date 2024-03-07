#include "../include/order_book.h"
#include "../include/order.h"

using namespace std;

std::random_device OrderBook::rd;
std::mt19937 OrderBook::gen(OrderBook::rd()); // Initialize gen with rd
std::uniform_int_distribution<unsigned int> OrderBook::distribution(0, std::numeric_limits<unsigned int>::max());

OrderBook::OrderBook(std::string security_in, unsigned int securityId_in) : security(security_in), securityId(securityId_in) {}

unsigned int OrderBook::addLimitOrder(double price_in, unsigned int size_in, bool buy) {
    if (!price_in || !size_in) return 0; // order_id of 0 implies unsuccessful operation
    unsigned int id = generateOrderId();
    std::shared_ptr<Order> addOrder = std::make_shared<Order>(id, size_in, OrderType::Limit, price_in, nullptr, nullptr, nullptr);
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

bool OrderBook::modifyLimitOrder(double price_in, unsigned int size_in, unsigned int orderId_in) {
    return false;
}

bool OrderBook::cancelLimitOrder(unsigned int orderId_in) { // could ask for more info from client to reduce server side overhead
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
    unsigned int randomNumber;
    do {
        randomNumber = distribution(gen);
    } while (allBuyOrders.contains(randomNumber) || allSellOrders.contains(randomNumber));

    return randomNumber;
}
