#!/bin/bash
# Runs inside the container. Each run gets a freshly started server, with its
# stdout discarded. Test sizes and run counts follow run_throughput_tests.sh.
#
# A failed RPC returns immediately, so a crashed server makes a run look fast.
# Every row therefore records the client errors and whether the server survived.
set -u

results=/results
versions=${VERSIONS:-"coarse fine current"}
throughput_runs=${THROUGHPUT_RUNS:-10}
latency_runs=${LATENCY_RUNS:-3}
max_size=${MAX_SIZE:-102400}
errors=$(mktemp)

mkdir -p "$results"
throughput_csv="$results/throughput.csv"
latency_csv="$results/latency_runs.csv"
[ -f "$throughput_csv" ] ||
    echo "Version,Test Size,Run,Duration (ms),Client Errors,Server" > "$throughput_csv"
[ -f "$latency_csv" ] ||
    echo "Version,Run,Client Errors,Server" > "$latency_csv"

start_server() {
    "/bench/$1/matching_engine_server" > /dev/null 2>&1 &
    server_pid=$!
    sleep 1
}

# Sets server_state to "alive", or to the exit status of a server that died.
stop_server() {
    if kill -0 "$server_pid" 2> /dev/null; then
        server_state=alive
        kill "$server_pid" 2> /dev/null
        wait "$server_pid" 2> /dev/null
    else
        wait "$server_pid" 2> /dev/null
        server_state="exit:$?"
    fi
}

for version in $versions; do
    for ((run = 1; run <= latency_runs; run++)); do
        echo "latency version=$version run=$run"
        start_server "$version"
        timeout 600 "/bench/$version/latency_test_1" 2> "$errors" \
            | awk '/^Latency measurements/ { getline; print }' \
            > "$results/latency_${version}_run${run}.txt"
        client_errors=$(grep -c 'Failed' "$errors")
        stop_server
        echo "$version,$run,$client_errors,$server_state" >> "$latency_csv"
    done

    for ((size = 100; size <= max_size; size *= 2)); do
        for ((run = 1; run <= throughput_runs; run++)); do
            echo "throughput version=$version size=$size run=$run"
            start_server "$version"
            duration=$(timeout 600 "/bench/$version/throughput_test_1" "$size" 2> "$errors" \
                | awk '/^Test Duration/ { print $3 }')
            client_errors=$(grep -c 'Failed' "$errors")
            stop_server
            echo "$version,$size,$run,${duration:-FAILED},$client_errors,$server_state" >> "$throughput_csv"
        done
    done
done
echo "done"
