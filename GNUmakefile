export BUILD ?= $(shell pwd)/build

run: build
	$(BUILD)/main

build: libr
	$(MAKE) -C src

libr:
	@mkdir -p "$(BUILD)"
	$(MAKE) -C libr install PREFIX="$(BUILD)/usr"

.PHONY: libr build
