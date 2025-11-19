#!/bin/bash

# Test script to demonstrate LangOS NFS functionality

echo "LangOS NFS System Test Script"
echo "============================="

# Start system in background
echo "Starting system..."
./start_system.sh &
SYSTEM_PID=$!
sleep 5

# Test basic operations
echo ""
echo "Running automated tests..."

# Function to run client commands
run_client_cmd() {
    echo "$1" | timeout 5 ./client 127.0.0.1 8000 <<< "testuser"
    sleep 1
}

echo ""
echo "1. Testing user registration and file creation..."
echo -e "testuser\nCREATE test.txt\nQUIT" | timeout 10 ./client 127.0.0.1 8000

sleep 2

echo ""
echo "2. Testing file listing..."
echo -e "testuser\nVIEW\nQUIT" | timeout 10 ./client 127.0.0.1 8000

sleep 2

echo ""
echo "3. Testing write and read operations..."
echo -e "testuser\nWRITE test.txt 0\n0 Hello world.\nETIRW\nREAD test.txt\nQUIT" | timeout 15 ./client 127.0.0.1 8000

echo ""
echo "Test completed. Check the output above for results."
echo "To manually test the system, run: ./client 127.0.0.1 8000"

# Stop system
kill $SYSTEM_PID 2>/dev/null
./stop_system.sh
