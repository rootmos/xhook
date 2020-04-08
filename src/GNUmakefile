CC = gcc
CFLAGS ?= -O1 -Wall -Werror
CFLAGS += -DLOG_LEVEL=LOG_DEBUG
LDFLAGS = -lrt -lr -lX11

CFLAGS += -I$(BUILD)/usr/include
LDFLAGS += -L$(BUILD)/usr/lib

all: $(foreach i, controller outline-current-window, $(BUILD)/$(i))

$(BUILD)/%: %.c
	$(CC) -o "$@" $(CFLAGS) $< $(LDFLAGS)

.PHONY: all
