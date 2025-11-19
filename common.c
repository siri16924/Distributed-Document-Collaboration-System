#include "common.h"

int create_server_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    if (listen(fd, 16) < 0) {
        perror("listen");
        exit(1);
    }
    return fd;
}

int connect_to_server(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

// Simple line-based IO: reads up to '\n'
int readline_fd(int fd, char *buf, size_t maxlen) {
    size_t i = 0;
    while (i < maxlen - 1) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n == 1) {
            if (c == '\n') {
                buf[i] = '\0';
                return (int)i;
            }
            buf[i++] = c;
        } else if (n == 0) {
            // EOF
            if (i == 0) return 0;
            buf[i] = '\0';
            return (int)i;
        } else {
            if (errno == EINTR) continue;
            return -1;
        }
    }
    buf[i] = '\0';
    return (int)i;
}

int writeline_fd(int fd, const char *buf) {
    size_t len = strlen(buf);
    size_t tot = 0;
    while (tot < len) {
        ssize_t n = write(fd, buf + tot, len - tot);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        tot += n;
    }
    // add newline
    if (write(fd, "\n", 1) != 1) return -1;
    return 0;
}
