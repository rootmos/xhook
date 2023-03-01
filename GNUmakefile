CC = gcc
CFLAGS ?= -O1 -Wall -Werror
LOG_LEVEL ?= INFO
CFLAGS += -DLOG_LEVEL=LOG_$(LOG_LEVEL)
LDFLAGS = -lX11

export PREFIX ?= $(HOME)/.local

define service
	envsubst < $(strip $(1)).service | install -D /dev/stdin $(HOME)/.config/systemd/user/$(strip $(1)).service
	systemctl --user enable $(strip $(1)).service
	systemctl --user restart $(strip $(1)).service
endef

.PHONY: run
run: build
	./xhook

.PHONY: install
install: build
	install --strip -D -t $(PREFIX)/bin xhook
	$(call service, xhook)

.PHONY: build
build: xhook

%: %.c r.h
	$(CC) -o $@ $(CFLAGS) $< $(LDFLAGS)

.PHONY: clean
clean:
	rm -f xhook
