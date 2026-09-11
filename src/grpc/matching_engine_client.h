#ifndef MATCHING_ENGINE_CLIENT_H
#define MATCHING_ENGINE_CLIENT_H

#include "matching_engine.grpc.pb.h"
#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <fstream>
#include <condition_variable>

class MatchingEngineClient {
public:
    MatchingEngineClient(const std::string& uri);

    unsigned int addOrder(const std::string& symbol, double price, unsigned int size, bool buy, bool limit = true);
    bool cancelOrder(const std::string& symbol, unsigned int orderID);
    void watchSymbol_print(const std::string& symbol, int watchDurationSeconds, bool log = false);
    void watchSymbol_data(const std::string& symbol, int watchDurationSeconds, std::mutex& dataMutex, std::condition_variable& cv, std::pair<double, double>& bestBidAsk, bool& dataAvailable);
    void getLevel2(const std::string& symbol, int depth, bool log, matchingengine::Level2Request::Side side);
    void subscribe(const std::string& symbol, int subscribeDurationSeconds);

private:
    std::unique_ptr<matchingengine::MatchingEngine::Stub> stub_;
};

void measureLatency(MatchingEngineClient& client, const std::string& symbol, double price, unsigned int size, bool buy);

#endif // MATCHING_ENGINE_CLIENT_H
