# LangOS Network File System (NFS)

A distributed file system implementation in C with support for concurrent access, sentence-level locking, and comprehensive file operations.

## Features

### Core Functionality
- **File Operations**: CREATE, READ, WRITE, DELETE with proper access control
- **Advanced Features**: 
  - Sentence-level locking for concurrent write operations
  - UNDO functionality to revert last changes
  - STREAM command for word-by-word file display
  - EXEC command to execute files as shell scripts
- **Access Control**: Owner-based file permissions with read/write access management
- **Metadata Management**: File statistics, timestamps, and ownership tracking
- **Multi-user Support**: Concurrent client connections with user authentication

### System Architecture
- **Name Server (NS)**: Central coordinator managing file metadata and storage server mapping
- **Storage Servers (SS)**: Distributed storage nodes handling actual file data
- **Clients**: User interface for interacting with the file system

## Quick Start

### Building the System
```bash
make clean && make
```

### Starting the System
```bash
# Start all components (NS + 3 Storage Servers)
./start_system.sh
```

### Connecting a Client
```bash
./client 127.0.0.1 8000
# Enter your username when prompted
```

### Stopping the System
```bash
./stop_system.sh
```

## Client Commands

### File Operations
```bash
VIEW                    # List accessible files
VIEW -a                 # List all files in system
VIEW -l                 # List files with detailed info
VIEW -al                # List all files with details

CREATE <filename>       # Create new empty file
READ <filename>         # Display file content
DELETE <filename>       # Delete file (owner only)
INFO <filename>         # Show file metadata
```

### Write Operations
```bash
WRITE <filename> <sentence_number>
# Enter write mode, then:
<word_index> <content>  # Insert/replace content at word position
ETIRW                   # Finish writing
```

Example:
```bash
WRITE test.txt 0
0 Hello world.
1 This is a test.
ETIRW
```

### Advanced Features
```bash
UNDO <filename>         # Revert last change
STREAM <filename>       # Stream content word-by-word (0.1s delay)
EXEC <filename>         # Execute file as shell script
```

### Access Control
```bash
ADDACCESS -R <filename> <username>  # Grant read access
ADDACCESS -W <filename> <username>  # Grant write access
REMACCESS <filename> <username>     # Remove access
```

### System Commands
```bash
LIST                    # List all users in system
HELP                    # Show command help
QUIT                    # Exit client
```

## File Format & Sentence Structure

- Files contain text organized into sentences
- Sentences are delimited by `.`, `!`, or `?`
- Words within sentences are space-separated
- Sentence-level locking prevents concurrent edits to the same sentence

## Architecture Details

### Name Server (Port 8000)
- Manages file metadata and location mapping
- Handles client authentication and authorization
- Coordinates with storage servers for file operations
- Implements efficient file lookup and caching

### Storage Servers
- **SS1**: NM Port 8001, Client Port 9001, Directory: ./data/ss1
- **SS2**: NM Port 8002, Client Port 9002, Directory: ./data/ss2  
- **SS3**: NM Port 8003, Client Port 9003, Directory: ./data/ss3

### Client Protocol
1. Connect to Name Server and register with username
2. Send file operation requests to Name Server
3. For direct operations (READ, WRITE, STREAM), Name Server provides Storage Server details
4. Client connects directly to appropriate Storage Server for data operations

## Error Handling

The system provides comprehensive error messages for:
- File not found
- Access denied
- Sentence locked by another user
- Storage server unavailable
- Invalid parameters

## Testing

### Automated Testing
```bash
./test_system.sh
```

### Manual Testing Examples
```bash
# Terminal 1: Start system
./start_system.sh

# Terminal 2: User 1
./client 127.0.0.1 8000
user1
CREATE shared.txt
WRITE shared.txt 0
0 This is a shared document.
ETIRW
ADDACCESS -W shared.txt user2
QUIT

# Terminal 3: User 2  
./client 127.0.0.1 8000
user2
READ shared.txt
WRITE shared.txt 1
0 User2 was here!
ETIRW
QUIT
```

## Implementation Highlights

### Concurrency Control
- Pthread-based threading for handling multiple clients
- Sentence-level locking prevents write conflicts
- Mutex protection for shared data structures

### Data Persistence
- File content stored in storage server directories
- Metadata files track ownership, permissions, and statistics
- Undo history maintained per file

### Network Communication
- TCP sockets for reliable client-server communication
- Line-based protocol for command exchange
- Direct client-to-storage-server connections for data operations

### Security
- User-based access control with read/write permissions
- Owner-only operations for file deletion and permission changes
- Input validation and error handling

## Project Structure
```
langos_skeleton/
├── nameserver.c        # Name server implementation
├── storageserver.c     # Storage server implementation  
├── client.c           # Client implementation
├── common.c           # Shared utilities
├── common.h           # Common definitions
├── Makefile           # Build configuration
├── start_system.sh    # System startup script
├── stop_system.sh     # System shutdown script
├── test_system.sh     # Automated test script
└── data/              # Storage directories
    ├── ss1/
    ├── ss2/
    └── ss3/
```

## Requirements Implemented

✅ **User Functionalities (150 marks)**
- VIEW files with flags support
- READ file content
- CREATE new files  
- WRITE with sentence-level locking
- UNDO last changes
- INFO file metadata display
- DELETE files (owner only)
- STREAM word-by-word content
- LIST all users
- ACCESS control (ADDACCESS/REMACCESS)
- EXEC file as shell script

✅ **System Requirements (40 marks)**
- Data persistence across server restarts
- Access control enforcement
- Comprehensive logging
- Error handling with clear messages
- Efficient file search and caching

✅ **Specifications (10 marks)**
- Proper initialization sequence
- Storage server registration
- Client authentication
- Direct client-storage communication

## Notes

- The system supports multiple concurrent users
- File operations are atomic with proper locking
- Storage servers can be added dynamically
- All operations are logged for debugging and monitoring
- The implementation follows the project specifications exactly

## Troubleshooting

**Connection Issues:**
- Ensure all ports (8000-8003, 9001-9003) are available
- Check that servers are running with `ps aux | grep nameserver`

**Permission Errors:**
- Verify file ownership and access permissions
- Use INFO command to check current file permissions

**Write Conflicts:**
- Another user may be editing the same sentence
- Try again after they finish (ETIRW) their write operation
