export BUILD ?= $(shell pwd)/build

run: build
	$(BUILD)/main -i /dev/input/event22

build: libr
	$(MAKE) -C src

libr:
	@mkdir -p "$(BUILD)"
	$(MAKE) -C libr install PREFIX="$(BUILD)/usr"

.PHONY: run libr build
