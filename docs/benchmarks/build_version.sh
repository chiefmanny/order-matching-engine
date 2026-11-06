#!/bin/sh
# Builds one engine version into /bench/<version>. The 2024 sources are left
# unmodified. Only their build files change, and newer compilers need the
# standard headers that those sources used to get transitively.
set -eu

version="$1"
src="/sources/$version"
cd "$src"

sed -i \
    -e 's#/Users/mananmanchanda/.local/bin#/usr/bin#g' \
    -e 's/set(CMAKE_BUILD_TYPE Debug)/set(CMAKE_BUILD_TYPE Release)/' \
    -e 's/find_package(Protobuf CONFIG REQUIRED)/find_package(Protobuf REQUIRED)/' \
    -E -e 's/^( *)# (add_executable\((latency_test_1|throughput_test_1) )/\1\2/' \
    -e 's/^( *)# (target_link_libraries\((latency_test_1|throughput_test_1) )/\1\2/' \
    CMakeLists.txt

headers="-include algorithm -include limits -include unordered_map -include vector -include iostream -include chrono -include mutex -include thread"
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="$headers"
cmake --build build -j"$(nproc)" --target matching_engine_server latency_test_1 throughput_test_1

mkdir -p "/bench/$version"
cp build/matching_engine_server build/latency_test_1 build/throughput_test_1 "/bench/$version/"
