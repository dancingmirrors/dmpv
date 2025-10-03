ifdef V
Q =
else
Q = @
endif

CFLAGS := -I$(ROOT) -I$(BUILD) $(CFLAGS)

OBJECTS = $(SOURCES:.c=.o)
OBJECTS := $(OBJECTS:.rc=.o)

TARGET = mpv

# The /./ -> / is for cosmetic reasons.
BUILD_OBJECTS = $(subst /./,/,$(addprefix $(BUILD)/, $(OBJECTS)))

BUILD_TARGET = $(addprefix $(BUILD)/, $(TARGET))$(EXESUF)
BUILD_DEPS = $(BUILD_OBJECTS:.o=.d)
CLEAN_FILES += $(BUILD_OBJECTS) $(BUILD_DEPS) $(BUILD_TARGET)

LOG = $(Q) printf "%s\t%s\n"

# Special rules.

all: $(BUILD_TARGET)

clean:
	$(LOG) "CLEAN"
	$(Q) rm -f $(CLEAN_FILES)
	$(Q) rm -rf $(BUILD)/generated/
	$(Q) (rmdir $(BUILD)/*/*/*  $(BUILD)/*/* $(BUILD)/*) 2> /dev/null || true

dist-clean:
	$(LOG) "DIST-CLEAN"
	$(Q) rm -rf $(BUILD)

install:
	$(LOG) "INSTALL"
	$(Q) mkdir -p -v /usr/local/bin
	$(Q) strip $(BUILD)/mpv
	$(Q) cp -v $(BUILD)/mpv /usr/local/bin
	$(Q) mkdir -p -v /usr/local/share/icons/hicolor/128x128/apps
	$(Q) cp -v etc/mpv-icon-8bit-128x128.png /usr/local/share/icons/hicolor/128x128/apps/mpv.png
	$(Q) mkdir -p -v /usr/local/share/applications
	$(Q) cp -v etc/mpv.desktop /usr/local/share/applications

install-msys2:
	$(LOG) "INSTALL-MSYS2"
	$(Q) strip.exe $(BUILD)/mpv.exe
	$(Q) cp -v $(BUILD)/mpv.exe /mingw64/bin

uninstall:
	$(LOG) "UNINSTALL"
	$(Q) rm -f -v /usr/local/bin/mpv
	$(Q) rm -f -v /usr/local/share/icons/hicolor/128x128/apps/mpv.png
	$(Q) rm -f -v /usr/local/share/applications/mpv.desktop

uninstall-msys2:
	$(LOG) "UNINSTALL-MSYS2"
	$(Q) rm -f -v /mingw64/bin/mpv.exe

# Generic pattern rules (used for most source files).

$(BUILD)/%.o: %.c
	$(LOG) "CC" "$@"
	$(Q) mkdir -p $(@D)
	$(Q) $(CC) $(CFLAGS) $< -c -o $@

$(BUILD)/%.o: %.rc
	$(LOG) "WINRC" "$@"
	$(Q) mkdir -p $(@D)
	$(Q) $(WINDRES) -I$(ROOT) -I$(BUILD) $< $@

$(BUILD_TARGET): $(BUILD_OBJECTS)
	$(LOG) "LINK" "$@"
	$(Q) $(CC) $(BUILD_OBJECTS) $(CFLAGS) $(LDFLAGS) -o $@

.PHONY: all clean .pregen

-include $(BUILD_DEPS)
