export BUILD ?= $(shell pwd)/build

export PREFIX ?= $(HOME)/.local

export INPUT_DEVICE_NAME ?= "SZMy-power LTD CO.  Dual Box WII"

define service
	envsubst < $(strip $(1)).service | install -D /dev/stdin $(HOME)/.config/systemd/user/$(strip $(1)).service
	systemctl --user enable $(strip $(1)).service
	systemctl --user restart $(strip $(1)).service
endef

run: build
	$(BUILD)/controller -n $(INPUT_DEVICE_NAME) -I 0

install: build
	install --strip -D -t $(PREFIX)/bin \
		$(BUILD)/controller $(BUILD)/outline-current-window $(BUILD)/xhook
	$(call service, controller)
	$(call service, xhook)

build: libr
	$(MAKE) -C src

libr:
	@mkdir -p "$(BUILD)"
	$(MAKE) -C libr install PREFIX="$(BUILD)/usr"

clean:
	rm -rf $(BUILD)

.PHONY: run install build clean libr
