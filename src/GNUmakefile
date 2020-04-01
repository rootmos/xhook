CC = gcc
CFLAGS ?= -O1 -Wall -Werror
CFLAGS += -DLOG_LEVEL=LOG_DEBUG
LDFLAGS = -lr -lX11

CFLAGS += -I$(BUILD)/usr/include
LDFLAGS += -L$(BUILD)/usr/lib

$(BUILD)/main: main.c
	$(CC) -o "$@" $(CFLAGS) $< $(LDFLAGS)
