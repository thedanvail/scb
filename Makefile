.PHONY: deps build test clean new-module nm

BUILD_DIR ?= build
MODULE_NAME := $(or $(NAME),$(filter-out deps build test clean new-module nm,$(MAKECMDGOALS)))

deps:
	git submodule update --init --recursive

build:
	cmake -S . -B $(BUILD_DIR) -DSCB_BUILD_TESTS=ON
	cmake --build $(BUILD_DIR)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	rm -rf $(BUILD_DIR)

new-module nm:
	@if [ -z "$(MODULE_NAME)" ]; then \
		echo "usage: make $@ <module-name>"; \
		echo "   or: make $@ NAME=<module-name>"; \
		exit 2; \
	fi
	@echo "$(MODULE_NAME)"

$(MODULE_NAME):
	@:
