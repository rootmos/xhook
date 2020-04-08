export BUILD ?= $(shell pwd)/build

TARGET = $(BUILD)/main
export PREFIX ?= $(HOME)/.local

export INPUT_DEVICE ?= /dev/input/event17

run: build
	$(TARGET) -i $(INPUT_DEVICE)

install: build
	install --strip -D $(TARGET) $(PREFIX)/bin/controller
	envsubst < controller.service \
		| install -D /dev/stdin $(HOME)/.config/systemd/user/controller.service
	systemctl --user enable controller.service
	systemctl --user restart controller.service

build: libr
	$(MAKE) -C src

libr:
	@mkdir -p "$(BUILD)"
	$(MAKE) -C libr install PREFIX="$(BUILD)/usr"

.PHONY: run install build libr
