#!/bin/bash

SERVER_EXEC="./build/matching_engine_server"
CLIENT_EXEC="./build/throughput_test_1"
OUTPUT_FILE="throughput_results.csv"

# Start the server in the background
function start_server() {
    $SERVER_EXEC &
    SERVER_PID=$!
    echo "Server started with PID $SERVER_PID"
}

# Stop the server
function stop_server() {
    echo "Stopping server with PID $SERVER_PID"
    kill $SERVER_PID
    wait $SERVER_PID
}

# Write the CSV header
echo "Test Size,Run,Duration (ms)" > $OUTPUT_FILE

# Run tests from 1 to 2^20, doubling each time
for (( size=100; size<=102400; size*=2 )); do
    for run in {1..10}; do
        echo "Running test with size=$size (Run #$run)"

        # Start the server
        start_server

        # Run the client and capture the output
        DURATION=$($CLIENT_EXEC $size | grep "Test Duration" | awk '{print $3}')
        
        # Stop the server
        stop_server

        # Save the result to the CSV file
        echo "$size,$run,$DURATION" >> $OUTPUT_FILE

        # Wait for a short time before the next run
        sleep 2
    done
done

echo "All tests completed. Results saved to $OUTPUT_FILE."