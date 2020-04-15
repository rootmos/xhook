CC = gcc
CFLAGS ?= -O1 -Wall -Werror
LOG_LEVEL ?= INFO
CFLAGS += -DLOG_LEVEL=LOG_$(LOG_LEVEL)
LDFLAGS = -lrt -lr -lX11

CFLAGS += -I$(BUILD)/usr/include
LDFLAGS += -L$(BUILD)/usr/lib

all: $(foreach i, controller outline-current-window, $(BUILD)/$(i))

$(BUILD)/%: %.c
	$(CC) -o "$@" $(CFLAGS) $< $(LDFLAGS)

.PHONY: all
