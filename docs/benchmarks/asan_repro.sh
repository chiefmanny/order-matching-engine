#!/bin/bash
# Runs inside the container. Builds each version with AddressSanitizer and puts
# it under the throughput test's load. Saves the first report from each failing
# run, because a dying server repeats its report across threads.
set -u

out=/results/asan
size=${ASAN_SIZE:-51200}
runs=${ASAN_RUNS:-3}
mkdir -p "$out"
echo "Version,Run,Size,Client Errors,Server,Sanitizer Report" > "$out/summary.csv"

headers="-include algorithm -include limits -include unordered_map -include vector -include iostream -include chrono -include mutex -include thread"
flags="$headers -fsanitize=address -fno-omit-frame-pointer -g -O1"

for version in fine coarse current; do
    cmake -S "/sources/$version" -B "/tmp/asan-$version" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address > /dev/null
    cmake --build "/tmp/asan-$version" -j"$(nproc)" \
        --target matching_engine_server throughput_test_1 > /dev/null

    for ((run = 1; run <= runs; run++)); do
        report=$(mktemp)
        ASAN_OPTIONS=detect_leaks=0 "/tmp/asan-$version/matching_engine_server" > /dev/null 2> "$report" &
        pid=$!
        sleep 2
        timeout 300 "/tmp/asan-$version/throughput_test_1" "$size" > /dev/null 2> /tmp/client.err
        client_errors=$(grep -c 'Failed' /tmp/client.err)
        if kill -0 "$pid" 2> /dev/null; then
            state=alive
            kill "$pid"
            wait "$pid" 2> /dev/null
        else
            wait "$pid" 2> /dev/null
            state="exit:$?"
        fi
        kind=$(grep -m1 -o 'AddressSanitizer: [a-z-]*' "$report" | cut -d' ' -f2)
        echo "$version,$run,$size,$client_errors,$state,${kind:-none}" >> "$out/summary.csv"
        if [ -s "$report" ] && [ ! -f "$out/${version}_report.txt" ]; then
            sed -n '1,/^SUMMARY/p' "$report" > "$out/${version}_report.txt"
        fi
    done
done
cat "$out/summary.csv"
