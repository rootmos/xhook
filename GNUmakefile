export BUILD ?= $(shell pwd)/build

export PREFIX ?= $(HOME)/.local

export INPUT_DEVICE_NAME ?= "SZMy-power LTD CO.  Dual Box WII"

run: build
	$(BUILD)/controller -n $(INPUT_DEVICE_NAME) -I 0

install: build
	install --strip -D -t $(PREFIX)/bin \
		$(BUILD)/controller $(BUILD)/outline-current-window
	envsubst < controller.service \
		| install -D /dev/stdin $(HOME)/.config/systemd/user/controller.service
	systemctl --user enable controller.service
	systemctl --user restart controller.service

build: libr
	$(MAKE) -C src

libr:
	@mkdir -p "$(BUILD)"
	$(MAKE) -C libr install PREFIX="$(BUILD)/usr"

clean:
	rm -rf $(BUILD)

.PHONY: run install build clean libr
