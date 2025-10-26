ifdef V
Q =
else
Q = @
endif

CFLAGS := -I$(ROOT) -I$(BUILD) $(CFLAGS)

OBJECTS = $(SOURCES:.c=.o)
OBJECTS := $(OBJECTS:.rc=.o)

TARGET = dmpv

# The /./ -> / is for cosmetic reasons.
BUILD_OBJECTS = $(subst /./,/,$(addprefix $(BUILD)/, $(OBJECTS)))

BUILD_TARGET = $(addprefix $(BUILD)/, $(TARGET))$(EXESUF)
BUILD_DEPS = $(BUILD_OBJECTS:.o=.d)
CLEAN_FILES += $(BUILD_OBJECTS) $(BUILD_DEPS) $(BUILD_TARGET)

LOG = $(Q) printf "%s\t%s\n"

PREFIX=/usr/local

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
	$(Q) mkdir -p ${PREFIX}/bin
	$(Q) cp -v $(BUILD)/dmpv ${PREFIX}/bin
	$(Q) mkdir -p ${PREFIX}/share/icons/hicolor/16x16/apps
	$(Q) mkdir -p ${PREFIX}/share/icons/hicolor/32x32/apps
	$(Q) mkdir -p ${PREFIX}/share/icons/hicolor/64x64/apps
	$(Q) mkdir -p ${PREFIX}/share/icons/hicolor/128x128/apps
	$(Q) mkdir -p ${PREFIX}/share/icons/hicolor/scalable/apps
	$(Q) mkdir -p ${PREFIX}/share/icons/hicolor/symbolic/apps
	$(Q) cp -v etc/dmpv-icon-8bit-16x16.png ${PREFIX}/share/icons/hicolor/16x16/apps/dmpv.png
	$(Q) cp -v etc/dmpv-icon-8bit-32x32.png ${PREFIX}/share/icons/hicolor/32x32/apps/dmpv.png
	$(Q) cp -v etc/dmpv-icon-8bit-64x64.png ${PREFIX}/share/icons/hicolor/64x64/apps/dmpv.png
	$(Q) cp -v etc/dmpv-icon-8bit-128x128.png ${PREFIX}/share/icons/hicolor/128x128/apps/dmpv.png
	$(Q) cp -v etc/dmpv.svg ${PREFIX}/share/icons/hicolor/scalable/apps/dmpv.svg
	$(Q) cp -v etc/dmpv-symbolic.svg ${PREFIX}/share/icons/hicolor/symbolic/apps/dmpv-symbolic.svg
	$(Q) mkdir -p ${PREFIX}/share/applications
	$(Q) cp -v etc/dmpv.desktop ${PREFIX}/share/applications
	$(Q) mkdir -p ${PREFIX}/etc
	$(Q) cp -v etc/dmpv.conf ${PREFIX}/etc

install-strip:
	$(LOG) "INSTALL-STRIP"
	$(Q) mkdir -p ${PREFIX}/bin
	$(Q) strip $(BUILD)/dmpv
	$(Q) cp -v $(BUILD)/dmpv ${PREFIX}/bin
	$(Q) mkdir -p ${PREFIX}/share/icons/hicolor/16x16/apps
	$(Q) mkdir -p ${PREFIX}/share/icons/hicolor/32x32/apps
	$(Q) mkdir -p ${PREFIX}/share/icons/hicolor/64x64/apps
	$(Q) mkdir -p ${PREFIX}/share/icons/hicolor/128x128/apps
	$(Q) mkdir -p ${PREFIX}/share/icons/hicolor/scalable/apps
	$(Q) mkdir -p ${PREFIX}/share/icons/hicolor/symbolic/apps
	$(Q) cp -v etc/dmpv-icon-8bit-16x16.png ${PREFIX}/share/icons/hicolor/16x16/apps/dmpv.png
	$(Q) cp -v etc/dmpv-icon-8bit-32x32.png ${PREFIX}/share/icons/hicolor/32x32/apps/dmpv.png
	$(Q) cp -v etc/dmpv-icon-8bit-64x64.png ${PREFIX}/share/icons/hicolor/64x64/apps/dmpv.png
	$(Q) cp -v etc/dmpv-icon-8bit-128x128.png ${PREFIX}/share/icons/hicolor/128x128/apps/dmpv.png
	$(Q) cp -v etc/dmpv.svg ${PREFIX}/share/icons/hicolor/scalable/apps/dmpv.svg
	$(Q) cp -v etc/dmpv-symbolic.svg ${PREFIX}/share/icons/hicolor/symbolic/apps/dmpv-symbolic.svg
	$(Q) mkdir -p ${PREFIX}/share/applications
	$(Q) cp -v etc/dmpv.desktop ${PREFIX}/share/applications
	$(Q) mkdir -p ${PREFIX}/etc
	$(Q) cp -v etc/dmpv.conf ${PREFIX}/etc

uninstall:
	$(LOG) "UNINSTALL"
	$(Q) rm -f -v ${PREFIX}/bin/dmpv
	$(Q) rm -f -v ${PREFIX}/share/icons/hicolor/16x16/apps/dmpv.png
	$(Q) rm -f -v ${PREFIX}/share/icons/hicolor/32x32/apps/dmpv.png
	$(Q) rm -f -v ${PREFIX}/share/icons/hicolor/64x64/apps/dmpv.png
	$(Q) rm -f -v ${PREFIX}/share/icons/hicolor/128x128/apps/dmpv.png
	$(Q) rm -f -v ${PREFIX}/share/icons/hicolor/scalable/apps/dmpv.svg
	$(Q) rm -f -v ${PREFIX}/share/icons/hicolor/symbolic/apps/dmpv-symbolic.svg
	$(Q) rm -f -v ${PREFIX}/share/applications/dmpv.desktop
	$(Q) rm -f -v ${PREFIXX}/etc/dmpv.conf

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
