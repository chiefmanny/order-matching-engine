#!/bin/bash
# Runs inside the container. Each run gets a freshly started server, with its
# stdout discarded. Test sizes and run counts follow run_throughput_tests.sh.
set -u

results=/results
versions=${VERSIONS:-"coarse fine current"}
throughput_runs=${THROUGHPUT_RUNS:-10}
latency_runs=${LATENCY_RUNS:-3}
max_size=${MAX_SIZE:-102400}

mkdir -p "$results"
csv="$results/throughput.csv"
[ -f "$csv" ] || echo "Version,Test Size,Run,Duration (ms)" > "$csv"

start_server() {
    "/bench/$1/matching_engine_server" > /dev/null 2>&1 &
    server_pid=$!
    sleep 1
}

stop_server() {
    kill "$server_pid" 2> /dev/null
    wait "$server_pid" 2> /dev/null
}

for version in $versions; do
    for ((run = 1; run <= latency_runs; run++)); do
        echo "latency version=$version run=$run"
        start_server "$version"
        timeout 600 "/bench/$version/latency_test_1" \
            | awk '/^Latency measurements/ { getline; print }' \
            > "$results/latency_${version}_run${run}.txt"
        stop_server
    done

    for ((size = 100; size <= max_size; size *= 2)); do
        for ((run = 1; run <= throughput_runs; run++)); do
            echo "throughput version=$version size=$size run=$run"
            start_server "$version"
            duration=$(timeout 600 "/bench/$version/throughput_test_1" "$size" \
                | awk '/^Test Duration/ { print $3 }')
            stop_server
            echo "$version,$size,$run,${duration:-FAILED}" >> "$csv"
        done
    done
done
echo "done"
