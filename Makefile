CC = gcc
CFLAGS = -Wall -Wextra -pthread

OBJS_COMMON = common.o

all: nameserver storageserver client

nameserver: nameserver.o $(OBJS_COMMON)
	$(CC) $(CFLAGS) -o nameserver nameserver.o $(OBJS_COMMON)

storageserver: storageserver.o $(OBJS_COMMON)
	$(CC) $(CFLAGS) -o storageserver storageserver.o $(OBJS_COMMON)

client: client.o $(OBJS_COMMON)
	$(CC) $(CFLAGS) -o client client.o $(OBJS_COMMON)

%.o: %.c common.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o nameserver storageserver client
