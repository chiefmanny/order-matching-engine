#ifndef matching_algorithm_h
#define matching_algorithm_h

#include "order.h"
#include <mutex>
#include <shared_mutex>
#include <thread>

using namespace std;

enum MatchingAlgorithmType {
    Simple
};

class MatchingAlgorithm {
    
    MatchingAlgorithm(std::shared_mutex& reader_in, std::shared_mutex& writer_in);
    
    std::shared_mutex& reader;
    std::shared_mutex& writer;
    
};

#endif /* matching_algorithm_h */
