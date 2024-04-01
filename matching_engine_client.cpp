//
//  matching_engine_server.cpp
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 3/31/24.
//

#include "build/matching_engine.grpc.pb.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;
using matchingengine::MatchingEngine;
using matchingengine::Order;
using matchingengine::OrderID;
using matchingengine::FillNotification;
using matchingengine::RejectedAddOrder;
using matchingengine::RejectedCancelOrder;
using matchingengine::Symbol;
using matchingengine::SymbolData;
using std::chrono::system_clock;

using namespace std;

Order makeOrder(const std::string& symbol, const double price, const unsigned int size, const bool buy, const bool limit = true) {
    Order order;
    order.set_symbol(symbol);
    order.set_price(price);
    order.set_size(size);
    order.set_buy(buy);
    order.set_limit(limit);
    return order;
}

OrderID makeOrderID(const std::string& symbol, const unsigned int id_in) {
    OrderID id;
    id.set_symbol(symbol);
    id.set_id(id_in);
    return id;
}

class Client {
public:
    Client(std::shared_ptr<Channel> channel) : stub_(MatchingEngine::NewStub(channel)) {}
    unsigned int addOrder(std::string symbol, double price, unsigned int size, bool buy, bool limit = true) {
        Order order;
        OrderID id;
        order = makeOrder(symbol, price, size, buy, limit);
        std::cout << "Client sending add order: " << order.symbol() << ", " << order.price() << ", " << order.size() << ", " << order.buy() << ", " << order.limit() << std::endl;
        if (sendAddOrder(order, &id)) {
            std::cout << "Client received add order response: " << id.id() << std::endl;
            return id.id();
        }
        return id.id();
    }
    bool cancelOrder(std::string symbol, unsigned int id) {
        OrderID id_send;
        OrderID id_receive;
        id_send = makeOrderID(symbol, id);
        std::cout << "Client sending cancel order: " << id_send.symbol() << ", " << id_send.id() << std::endl;
        if (sendCancelOrder(id_send, &id_receive)) {
            std::cout << "Client received cancel order response: " << id_receive.symbol() << ", " << id_receive.id() << std::endl;
            return true;
        }
        return false;
    }
private:
    bool sendAddOrder(const Order& order, OrderID* id) {
        ClientContext context;
        Status status = stub_->AddLimitOrder(&context, order, id);
        if (!status.ok()) {
            std::cout << "AddLimitOrder rpc failed." << std::endl;
            return false;
        }
        else {
            std::cout << "Added order, received order ID: " << id->id() << std::endl;
        }
        return true;
    }
    bool sendCancelOrder(const OrderID& id_send, OrderID* id_receive) {
        ClientContext context;
        Status status = stub_->CancelLimitOrder(&context, id_send, id_receive);
        if (!status.ok()) {
            std::cout << "CancelLimitOrder rpc failed." << std::endl;
            return false;
        }
        else {
            std::cout << "Cancelled order, received order ID: " << id_receive->id() << std::endl;
        }
        return true;
    }
    std::unique_ptr<MatchingEngine::Stub> stub_;
};

int main(int argc, char** argv) {
    Client first(grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials()));

    unsigned int a = first.addOrder("GME", 12.5, 100, true);
    unsigned int b = first.addOrder("GME", 12.6, 200, true);
    unsigned int c = first.addOrder("GME", 12.7, 600, true);

    unsigned int x = first.addOrder("GME", 12.8, 600, false);
    unsigned int y = first.addOrder("GME", 12.9, 200, false);
    unsigned int z = first.addOrder("GME", 13.0, 100, false);

    first.cancelOrder("GME", a);
    first.cancelOrder("GME", z);
    return 0;
}