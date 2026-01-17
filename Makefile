# Pass CCACHE=no to configure to disable ccache.

BUILDDIR = build

NINJA := $(shell command -v ninja 2> /dev/null)

BUILD_EXISTS := $(shell test -d $(BUILDDIR) && echo yes)

.DEFAULT_GOAL := all

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
