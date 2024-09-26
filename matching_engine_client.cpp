//
//  matching_engine_server.cpp
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 3/31/24.
//

#include "build/matching_engine.grpc.pb.h"
#include "matching_engine_client.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <fstream>
#include <condition_variable>

#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using matchingengine::MatchingEngine;
using matchingengine::Order;
using matchingengine::OrderID;
using std::chrono::high_resolution_clock;
using std::chrono::duration_cast;
using std::chrono::milliseconds;

using namespace std;

MatchingEngineClient::MatchingEngineClient(const std::string& uri)
    : stub_(MatchingEngine::NewStub(grpc::CreateChannel(uri, grpc::InsecureChannelCredentials()))) {}

unsigned int MatchingEngineClient::addOrder(const std::string& symbol, double price, unsigned int size, bool buy, bool limit) {
    Order order;
    order.set_symbol(symbol);
    order.set_price(price);
    order.set_size(size);
    order.set_buy(buy);
    order.set_limit(limit);

    OrderID orderID;
    ClientContext context;
    Status status = stub_->AddLimitOrder(&context, order, &orderID);
    if (!status.ok()) {
        std::cerr << "Failed to add order: " << status.error_message() << std::endl;
        return 0;
    }
    std::cout << "Order added with ID: " << orderID.id() << std::endl;
    return orderID.id();
}

bool MatchingEngineClient::cancelOrder(const std::string& symbol, unsigned int orderID) {
    OrderID request;
    request.set_symbol(symbol);
    request.set_id(orderID);

    OrderID response;
    grpc::ClientContext context;
    grpc::Status status = stub_->CancelLimitOrder(&context, request, &response);
    if (!status.ok()) {
        std::cerr << "Failed to cancel order: " << status.error_message() << std::endl;
        return false;
    }
    std::cout << "Order canceled: " << response.id() << std::endl;
    return true;
}

void MatchingEngineClient::watchSymbol_print(const std::string& symbol, int watchDurationSeconds, bool log) {
    std::thread::id this_id = std::this_thread::get_id();
    grpc::ClientContext context;
    matchingengine::Symbol symbolRequest;
    symbolRequest.set_symbol(symbol);

    std::unique_ptr<grpc::ClientReader<matchingengine::SymbolData>> reader(
        stub_->WatchSymbol(&context, symbolRequest)
    );

    matchingengine::SymbolData data;
    auto startTime = std::chrono::steady_clock::now();
    std::ofstream outFile;

    if (log) {
        outFile.open(symbol + "_market_data.csv");
        outFile << "bid,bidSize,ask,askSize\n";  // CSV header
    }

    while (reader->Read(&data)) {
        // Process the data received from the server
        std::cout << "Thread ID " << this_id << ": " << "Bid: " << data.lastbid() << " Size: " << data.lastbidsize()
                  << ", Ask: " << data.lastask() << " Size: " << data.lastasksize() << std::endl;

        // Log the data to a CSV file if log is true
        if (log) {
            outFile << data.lastbid() << "," << data.lastbidsize() << ","
                    << data.lastask() << "," << data.lastasksize() << "\n";
        }

        // Check if the watch duration has elapsed
        auto currentTime = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime).count() >= watchDurationSeconds) {
            // Signal the server to stop sending data
            context.TryCancel();
            break;
        }
    }

    grpc::Status status = reader->Finish();
    if (!status.ok()) {
        std::cerr << "WatchSymbol stream ended with error: " << status.error_message() << std::endl;
    } else {
        std::cout << "Finished watching symbol " << symbol << " for " << watchDurationSeconds << " seconds." << std::endl;
    }

    // Close the file if logging
    if (log) {
        outFile.close();
        std::cout << "Data saved to " << symbol + "_market_data.csv" << std::endl;
    }
}

void MatchingEngineClient::watchSymbol_data(const std::string& symbol, int watchDurationSeconds, 
                                       std::mutex& dataMutex, std::condition_variable& cv, 
                                       std::pair<double, double>& bestBidAsk, bool& dataAvailable) {
    grpc::ClientContext context;
    matchingengine::Symbol symbolRequest;
    symbolRequest.set_symbol(symbol);

    std::unique_ptr<grpc::ClientReader<matchingengine::SymbolData>> reader(
        stub_->WatchSymbol(&context, symbolRequest)
    );

    matchingengine::SymbolData data;
    auto startTime = std::chrono::steady_clock::now();

    while (reader->Read(&data)) {
        {
            // Lock the mutex to safely update the shared best bid/ask
            std::lock_guard<std::mutex> lock(dataMutex);
            bestBidAsk.first = data.lastbid();  // Update the best bid
            bestBidAsk.second = data.lastask();  // Update the best ask
            dataAvailable = true;  // Signal that new data is available
        }

        // Notify the trading threads that new data is available
        cv.notify_all();

        // Check if the watch duration has elapsed
        auto currentTime = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime).count() >= watchDurationSeconds) {
            context.TryCancel();
            break;
        }
    }

    grpc::Status status = reader->Finish();
    if (!status.ok()) {
        std::cerr << "WatchSymbol stream ended with error: " << status.error_message() << std::endl;
    }
}

void MatchingEngineClient::getLevel2(const std::string& symbol, int depth, bool log, matchingengine::Level2Request::Side side) {
    grpc::ClientContext context;
    matchingengine::Level2Request request;
    request.set_symbol(symbol);
    request.set_depth(depth);
    request.set_side(side);

    std::unique_ptr<grpc::ClientReader<matchingengine::Level2Response>> reader(
        stub_->GetLevel2(&context, request)
    );

    matchingengine::Level2Response response;
    std::ofstream outFile;
    if (log) {
        outFile.open(symbol + "_level2_data.csv");
        outFile << "side,price,size,rank\n";  // CSV header
    }

    // Read the streamed response
    while (reader->Read(&response)) {
        // Process the bids
        for (const auto& bid : response.bids()) {
            std::cout << "Bid - Price: " << bid.price() << ", Size: " << bid.size() << ", Rank: " << bid.rank() << std::endl;

            if (log) {
                outFile << "BID," << bid.price() << "," << bid.size() << "," << bid.rank() << "\n";
            }
        }

        // Process the asks
        for (const auto& ask : response.asks()) {
            std::cout << "Ask - Price: " << ask.price() << ", Size: " << ask.size() << ", Rank: " << ask.rank() << std::endl;

            if (log) {
                outFile << "ASK," << ask.price() << "," << ask.size() << "," << ask.rank() << "\n";
            }
        }
    }

    grpc::Status status = reader->Finish();
    if (!status.ok()) {
        std::cerr << "GetLevel2 stream ended with error: " << status.error_message() << std::endl;
    } else {
        std::cout << "Finished fetching level 2 data for symbol " << symbol << std::endl;
    }

    if (log) {
        outFile.close();
        std::cout << "Level 2 data saved to " << symbol + "_level2_data.csv" << std::endl;
    }
}

void measureLatency(MatchingEngineClient& client, const std::string& symbol, double price, unsigned int size, bool buy) {
    auto start = std::chrono::steady_clock::now();
    unsigned int orderId = client.addOrder(symbol, price, size, buy);
    auto end = std::chrono::steady_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    std::cout << "Latency: " << duration << " microseconds\n";
}