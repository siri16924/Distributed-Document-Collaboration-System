#!/bin/bash

# LangOS NFS System Startup Script

echo "Starting LangOS Network File System..."

# Create data directories
mkdir -p data/ss1 data/ss2 data/ss3

# Kill any existing processes
pkill -f nameserver
pkill -f storageserver
sleep 1

# Start Name Server
echo "Starting Name Server on port 8000..."
./nameserver 8000 &
NAMESERVER_PID=$!
sleep 2

# Start Storage Servers
echo "Starting Storage Server 1..."
./storageserver 127.0.0.1 8000 8001 9001 ./data/ss1 &
SS1_PID=$!
sleep 1

echo "Starting Storage Server 2..."
./storageserver 127.0.0.1 8000 8002 9002 ./data/ss2 &
SS2_PID=$!
sleep 1

echo "Starting Storage Server 3..."
./storageserver 127.0.0.1 8000 8003 9003 ./data/ss3 &
SS3_PID=$!
sleep 1

echo ""
echo "===================================="
echo "LangOS NFS System is now running!"
echo "===================================="
echo ""
echo "Name Server: Running on port 8000 (PID: $NAMESERVER_PID)"
echo "Storage Server 1: NM port 8001, Client port 9001 (PID: $SS1_PID)"
echo "Storage Server 2: NM port 8002, Client port 9002 (PID: $SS2_PID)" 
echo "Storage Server 3: NM port 8003, Client port 9003 (PID: $SS3_PID)"
echo ""
echo "To connect a client, run:"
echo "  ./client 127.0.0.1 8000"
echo ""
echo "To stop the system, run:"
echo "  ./stop_system.sh"
echo ""

# Wait for user input
echo "Press Ctrl+C to stop all servers..."
trap 'echo ""; echo "Stopping all servers..."; kill $NAMESERVER_PID $SS1_PID $SS2_PID $SS3_PID 2>/dev/null; exit 0' INT
wait
