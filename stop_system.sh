#!/bin/bash

echo "Stopping LangOS NFS System..."

# Kill all processes
pkill -f nameserver
pkill -f storageserver

echo "All servers stopped."
