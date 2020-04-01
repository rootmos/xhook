CC = gcc
CFLAGS ?= -O1 -Wall -Werror
LDFLAGS = -lr

CFLAGS += -I$(BUILD)/usr/include
LDFLAGS += -L$(BUILD)/usr/lib

$(BUILD)/main: main.c
	$(CC) -o "$@" $(CFLAGS) $< $(LDFLAGS)
