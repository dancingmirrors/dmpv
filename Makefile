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

# Helper function to validate environment and redirect to ninja
define REDIRECT_TO_NINJA
	@if [ -z "$(NINJA)" ]; then \
		echo "Error: 'ninja' not found. Please install Ninja."; \
		exit 1; \
	fi
	@if [ "$(BUILD_EXISTS)" != "yes" ]; then \
		echo "Error: build directory not found. Please run ./configure first."; \
		exit 1; \
	fi
	@echo "Redirecting to: ninja -C $(BUILDDIR) $(1)"
	@$(NINJA) -C $(BUILDDIR) $(1)
endef

# Redirect all common targets to ninja
.PHONY: all clean install uninstall

all:
	$(call REDIRECT_TO_NINJA,)

clean:
	$(call REDIRECT_TO_NINJA,clean)

install:
	$(call REDIRECT_TO_NINJA,install)

uninstall:
	$(call REDIRECT_TO_NINJA,uninstall)

# Catch-all target to redirect any other make target to ninja
%:
	$(call REDIRECT_TO_NINJA,$@)
