#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>

#define MAX_LINE 4096
#define MAX_USERNAME 64
#define MAX_FILENAME 256
#define MAX_USERS 128
#define MAX_FILES 1024
#define MAX_SS 32

// ---- Error codes (universal) ----
typedef enum {
    ERR_OK = 0,
    ERR_NOT_FOUND = 1,
    ERR_NO_ACCESS = 2,
    ERR_LOCKED = 3,
    ERR_INVALID = 4,
    ERR_INTERNAL = 5,
    ERR_ALREADY_EXISTS = 6,
    ERR_SS_DOWN = 7
} ErrorCode;

// ---- Access flags ----
#define ACCESS_READ  0x1
#define ACCESS_WRITE 0x2

typedef struct {
    char username[MAX_USERNAME];
    int access_flags;  // bitmask: R/W
} FileACL;

typedef struct {
    char filename[MAX_FILENAME];
    char owner[MAX_USERNAME];
    int word_count;
    int char_count;
    time_t created_at;
    time_t last_modified;
    time_t last_access;
    char last_access_user[MAX_USERNAME];

    int acl_count;
    FileACL acl[32];

    // location:
    int ss_id;          // index in NM’s storage server table
} FileMeta;

// Storage Server meta at NM
typedef struct {
    int id;
    char ip[64];
    int port_nm;        // port used for NM<->SS communication
    int port_client;    // port for client<->SS
    int alive;          // 1 if alive
} StorageServerInfo;

// ---- Simple socket helpers ----
int create_server_socket(int port);
int connect_to_server(const char *ip, int port);
int readline_fd(int fd, char *buf, size_t maxlen);
int writeline_fd(int fd, const char *buf);

#endif
