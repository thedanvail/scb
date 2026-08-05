.PHONY: all deps build test clean health new-module nm

BUILD_DIR ?= build
MODULE_NAME := $(or $(NAME),$(filter-out all deps build test clean health new-module nm,$(MAKECMDGOALS)))

all: build

define ensure_build_dir
	@dir="$(strip $(1))"; \
	if [ -z "$$dir" ]; then \
		echo "refusing to remove an empty BUILD_DIR"; \
		exit 2; \
	fi; \
	current_root=$$(pwd -P); \
	source_parent=$$(dirname -- "$$current_root"); \
	if [ -d "$$dir" ]; then \
		dir_abs=$$(cd "$$dir" 2>/dev/null && pwd -P) || { \
			echo "refusing to use inaccessible build directory: $$dir"; \
			exit 2; \
		}; \
	else \
		parent=$$(dirname -- "$$dir"); \
		base=$$(basename -- "$$dir"); \
		parent_abs=$$(cd "$$parent" 2>/dev/null && pwd -P) || { \
			echo "refusing to use build directory with missing parent: $$dir"; \
			exit 2; \
		}; \
		dir_abs="$$parent_abs/$$base"; \
	fi; \
	if [ "$$dir_abs" = "$$current_root" ] || [ "$$dir_abs" = "$$source_parent" ] || [ "$$dir_abs" = "/" ]; then \
		echo "refusing dangerous build directory: $$dir"; \
		exit 2; \
	fi; \
	if [ -f "$$dir_abs/CMakeCache.txt" ]; then \
		cache_root=$$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$$dir_abs/CMakeCache.txt"); \
		cache_root_abs=$$(cd "$$cache_root" 2>/dev/null && pwd -P || printf '%s' "$$cache_root"); \
		if [ "$$cache_root_abs" != "$$current_root" ]; then \
			echo "Removing stale CMake cache in $$dir_abs"; \
			echo "  cache root: $$cache_root_abs"; \
			echo "  current root: $$current_root"; \
			rm -rf -- "$$dir_abs"; \
		fi; \
	fi
endef

deps:
	git submodule update --init --recursive

build:
	$(call ensure_build_dir,$(BUILD_DIR))
	cmake -S . -B $(BUILD_DIR) -DSCB_BUILD_TESTS=ON
	cmake --build $(BUILD_DIR)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

health:
	$(call ensure_build_dir,$(BUILD_DIR))
	cmake -S . -B $(BUILD_DIR) -DSCB_BUILD_TESTS=ON
	cmake --build $(BUILD_DIR)
	ctest --test-dir $(BUILD_DIR) --output-on-failure -R 'scb_cli_(version|build_help|invalid_usage|missing_manifest|clean_missing_manifest_path|clean_invalid_manifest|clean_missing_manifest)'
	$(call ensure_build_dir,$(BUILD_DIR)-no-tests)
	cmake -S . -B $(BUILD_DIR)-no-tests -DSCB_BUILD_TESTS=OFF

clean:
	$(call ensure_build_dir,$(BUILD_DIR))
	$(call ensure_build_dir,$(BUILD_DIR)-no-tests)
	rm -rf -- "$(BUILD_DIR)" "$(BUILD_DIR)-no-tests"

new-module nm:
	@if [ -z "$(MODULE_NAME)" ]; then \
		echo "usage: make $@ <module-name>"; \
		echo "   or: make $@ NAME=<module-name>"; \
		exit 2; \
	fi
	@echo "$(MODULE_NAME)"

$(MODULE_NAME):
	@:
