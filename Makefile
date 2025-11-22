# Build system:
# This project uses Ninja as the build backend for faster builds.
#
# Quick start:
# ./configure
# ninja -v -C build
#
# Build performance tips:
# - Ninja automatically uses parallel builds
# - ccache is automatically detected and enabled if installed (works with both gcc and clang)
# - ccache works even when setting CC explicitly: CC=clang ./configure
# - To disable ccache: CCACHE=no ./configure (or CCACHE=no CC=clang ./configure)
# - Disable debug symbols for faster compilation: ./configure --disable-debug-build && ninja -v -C build
#
# Note: Direct 'make' usage is no longer supported. All make commands now redirect to ninja with verbose output.

BUILDDIR = build

# Check if ninja is available
NINJA := $(shell command -v ninja 2> /dev/null)

# Check if build directory exists
BUILD_EXISTS := $(shell test -d $(BUILDDIR) && echo yes)

.DEFAULT_GOAL := all

# Redirect all common targets to ninja
.PHONY: all clean install uninstall

all:
	@if [ -z "$(NINJA)" ]; then \
		echo "Error: 'ninja' not found. Please install Ninja."; \
		exit 1; \
	fi
	@if [ "$(BUILD_EXISTS)" != "yes" ]; then \
		echo "Error: build directory not found. Please run ./configure first."; \
		exit 1; \
	fi
	@echo "Redirecting to: ninja -v -C $(BUILDDIR)"
	@$(NINJA) -v -C $(BUILDDIR)

clean:
	@if [ -z "$(NINJA)" ]; then \
		echo "Error: 'ninja' not found. Please install Ninja."; \
		exit 1; \
	fi
	@if [ "$(BUILD_EXISTS)" != "yes" ]; then \
		echo "Error: build directory not found. Please run ./configure first."; \
		exit 1; \
	fi
	@echo "Redirecting to: ninja -v -C $(BUILDDIR) clean"
	@$(NINJA) -v -C $(BUILDDIR) clean

install:
	@if [ -z "$(NINJA)" ]; then \
		echo "Error: 'ninja' not found. Please install Ninja."; \
		exit 1; \
	fi
	@if [ "$(BUILD_EXISTS)" != "yes" ]; then \
		echo "Error: build directory not found. Please run ./configure first."; \
		exit 1; \
	fi
	@echo "Redirecting to: ninja -v -C $(BUILDDIR) install"
	@$(NINJA) -v -C $(BUILDDIR) install

uninstall:
	@if [ -z "$(NINJA)" ]; then \
		echo "Error: 'ninja' not found. Please install Ninja."; \
		exit 1; \
	fi
	@if [ "$(BUILD_EXISTS)" != "yes" ]; then \
		echo "Error: build directory not found. Please run ./configure first."; \
		exit 1; \
	fi
	@echo "Redirecting to: ninja -v -C $(BUILDDIR) uninstall"
	@$(NINJA) -v -C $(BUILDDIR) uninstall

# Catch-all target to redirect any other make target to ninja
%:
	@if [ -z "$(NINJA)" ]; then \
		echo "Error: 'ninja' not found. Please install Ninja."; \
		exit 1; \
	fi
	@if [ "$(BUILD_EXISTS)" != "yes" ]; then \
		echo "Error: build directory not found. Please run ./configure first."; \
		exit 1; \
	fi
	@echo "Redirecting to: ninja -v -C $(BUILDDIR) $@"
	@$(NINJA) -v -C $(BUILDDIR) $@
