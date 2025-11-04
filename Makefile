# Build system:
# This project uses Ninja as the build backend for faster builds.
# 
# Quick start:
# ./configure
# ninja -C build
#
# Build performance tips:
# - Ninja automatically uses parallel builds
# - Use ccache for faster rebuilds: CC="ccache gcc" ./configure && ninja -C build
# - Disable debug symbols for faster compilation: ./configure --disable-debug-build && ninja -C build
#
# Note: Direct 'make' usage is no longer supported. All make commands now redirect to ninja.

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
		echo "Error: ninja not found. Please install ninja build system."; \
		echo "On Ubuntu/Debian: sudo apt-get install ninja-build"; \
		echo "On Fedora: sudo dnf install ninja-build"; \
		echo "On macOS: brew install ninja"; \
		exit 1; \
	fi
	@if [ "$(BUILD_EXISTS)" != "yes" ]; then \
		echo "Error: build directory not found. Please run ./configure first."; \
		exit 1; \
	fi
	@echo "Redirecting to: ninja -C $(BUILDDIR)"
	@$(NINJA) -C $(BUILDDIR)

clean:
	@if [ -z "$(NINJA)" ]; then \
		echo "Error: ninja not found. Please install ninja build system."; \
		exit 1; \
	fi
	@if [ "$(BUILD_EXISTS)" != "yes" ]; then \
		echo "Error: build directory not found. Please run ./configure first."; \
		exit 1; \
	fi
	@echo "Redirecting to: ninja -C $(BUILDDIR) clean"
	@$(NINJA) -C $(BUILDDIR) clean

install:
	@if [ -z "$(NINJA)" ]; then \
		echo "Error: ninja not found. Please install ninja build system."; \
		exit 1; \
	fi
	@if [ "$(BUILD_EXISTS)" != "yes" ]; then \
		echo "Error: build directory not found. Please run ./configure first."; \
		exit 1; \
	fi
	@echo "Redirecting to: ninja -C $(BUILDDIR) install"
	@$(NINJA) -C $(BUILDDIR) install

uninstall:
	@if [ -z "$(NINJA)" ]; then \
		echo "Error: ninja not found. Please install ninja build system."; \
		exit 1; \
	fi
	@if [ "$(BUILD_EXISTS)" != "yes" ]; then \
		echo "Error: build directory not found. Please run ./configure first."; \
		exit 1; \
	fi
	@echo "Redirecting to: ninja -C $(BUILDDIR) uninstall"
	@$(NINJA) -C $(BUILDDIR) uninstall

# Catch-all target to redirect any other make target to ninja
%:
	@if [ -z "$(NINJA)" ]; then \
		echo "Error: ninja not found. Please install ninja build system."; \
		exit 1; \
	fi
	@if [ "$(BUILD_EXISTS)" != "yes" ]; then \
		echo "Error: build directory not found. Please run ./configure first."; \
		exit 1; \
	fi
	@echo "Redirecting to: ninja -C $(BUILDDIR) $@"
	@$(NINJA) -C $(BUILDDIR) $@
