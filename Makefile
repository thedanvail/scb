.PHONY: deps build test clean health new-module nm

BUILD_DIR ?= build
MODULE_NAME := $(or $(NAME),$(filter-out deps build test clean health new-module nm,$(MAKECMDGOALS)))

define ensure_build_dir
	@if [ -f "$(BUILD_DIR)/CMakeCache.txt" ]; then \
		cache_root=$$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$(BUILD_DIR)/CMakeCache.txt"); \
		current_root=$$(pwd); \
		if [ "$$cache_root" != "$$current_root" ]; then \
			echo "Removing stale CMake cache in $(BUILD_DIR)"; \
			echo "  cache root: $$cache_root"; \
			echo "  current root: $$current_root"; \
			rm -rf "$(BUILD_DIR)"; \
		fi; \
	fi
endef

deps:
	git submodule update --init --recursive

build:
	$(ensure_build_dir)
	cmake -S . -B $(BUILD_DIR) -DSCB_BUILD_TESTS=ON
	cmake --build $(BUILD_DIR)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

health:
	$(ensure_build_dir)
	cmake -S . -B $(BUILD_DIR) -DSCB_BUILD_TESTS=ON
	cmake --build $(BUILD_DIR)
	ctest --test-dir $(BUILD_DIR) --output-on-failure -R 'scb_cli_(version|build_help|invalid_usage|missing_manifest|clean_missing_manifest_path|clean_invalid_manifest|clean_missing_manifest)'
	cmake -S . -B $(BUILD_DIR)-no-tests -DSCB_BUILD_TESTS=OFF

clean:
	rm -rf $(BUILD_DIR) $(BUILD_DIR)-no-tests

new-module nm:
	@if [ -z "$(MODULE_NAME)" ]; then \
		echo "usage: make $@ <module-name>"; \
		echo "   or: make $@ NAME=<module-name>"; \
		exit 2; \
	fi
	@echo "$(MODULE_NAME)"

$(MODULE_NAME):
	@:
