//
//  matching_algorithm.cpp
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 3/8/24.
//

#include <stdio.h>
#include "../include/matching_algorithm.h"

MatchingAlgorithm::MatchingAlgorithm(std::shared_mutex& reader_in, std::shared_mutex& writer_in) : reader(reader_in), writer(writer_in) {}
