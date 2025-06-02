BUILDDIR = build

include $(BUILDDIR)/config.mak
include $(ROOT)/TOOLS/makefile_common.mak

PROJNAME = mpv

.PHONY: .force

$(BUILD)/generated/version.h: $(ROOT)/version.sh .force
	$(LOG) "VERSION" $@
	$(Q) mkdir -p $(@D)
	$(Q) $(ROOT)/version.sh --versionh=$@

$(BUILD)/generated/ebml_types.h $(BUILD)/generated/ebml_defs.c: $(ROOT)/TOOLS/matroska.py
	$(LOG) "EBML" "$(BUILD)/generated/ebml_types.h $(BUILD)/generated/ebml_defs.c"
	$(Q) mkdir -p $(@D)
	$(Q) $< --generate-header > $(BUILD)/generated/ebml_types.h
	$(Q) $< --generate-definitions > $(BUILD)/generated/ebml_defs.c

$(BUILD)/generated/%.inc: $(ROOT)/TOOLS/file2string.py $(ROOT)/%
	$(LOG) "INC" $@
	$(Q) mkdir -p $(@D)
	$(Q) $^ > $@

# Dependencies for generated files unfortunately need to be declared manually.
# This is because dependency scanning is a gross shitty hack by concept, and
# requires that the compiler successfully compiles a file to get its
# dependencies. This results in a chicken-and-egg problem, and in conclusion
# it works for static header files only.
# If any headers include generated headers, you need to manually set
# dependencies on all source files that include these headers!
# And because make is fucking shit, you actually need to set these on all files
# that are generated from these sources, instead of the source files. Make rules
# specify recipes, not dependencies.
# (Possible counter measures: always generate them with an order dependency, or
#  introduce a separate dependency scanner step for creating .d files.)

$(BUILD)/common/version.o: $(BUILD)/generated/version.h

$(BUILD)/osdep/mpv.o: $(BUILD)/generated/version.h

$(BUILD)/demux/demux_mkv.o $(BUILD)/demux/ebml.o: \
    $(BUILD)/generated/ebml_types.h $(BUILD)/generated/ebml_defs.c

$(BUILD)/video/out/x11_common.o: $(BUILD)/generated/etc/mpv-icon-8bit-16x16.png.inc \
                                 $(BUILD)/generated/etc/mpv-icon-8bit-32x32.png.inc \
                                 $(BUILD)/generated/etc/mpv-icon-8bit-64x64.png.inc \
                                 $(BUILD)/generated/etc/mpv-icon-8bit-128x128.png.inc

$(BUILD)/input/input.o: $(BUILD)/generated/etc/input.conf.inc

$(BUILD)/player/main.o: $(BUILD)/generated/etc/builtin.conf.inc

$(BUILD)/sub/osd_libass.o: $(BUILD)/generated/sub/osd_font.otf.inc

$(BUILD)/osdep/macosx_application.m $(BUILD)/video/out/cocoa_common.m: \
    $(BUILD)/generated/TOOLS/osxbundle/mpv.app/Contents/Resources/icon.icns.inc

# $(1): path prefix to the protocol, $(1)/$(2).xml is the full path.
# $(2): the name of the protocol, without path or extension
define generate_wayland =
$$(BUILD)/video/out/wayland_common.o \
$$(BUILD)/video/out/opengl/context_wayland.o \
: $$(BUILD)/generated/wayland/$(2).c $$(BUILD)/generated/wayland/$(2).h
$$(BUILD)/generated/wayland/$(2).c: $(1)/$(2).xml
	$$(LOG) "WAYSHC" $$@
	$$(Q) mkdir -p $$(@D)
	$$(Q) $$(WAYSCAN) private-code $$< $$@
$$(BUILD)/generated/wayland/$(2).h: $(1)/$(2).xml
	$$(LOG) "WAYSHH" $$@
	$$(Q) mkdir -p $$(@D)
	$$(Q) $$(WAYSCAN) client-header $$< $$@
endef

$(eval $(call generate_wayland,$(WL_PROTO_DIR)/unstable/idle-inhibit/,idle-inhibit-unstable-v1))
$(eval $(call generate_wayland,$(WL_PROTO_DIR)/stable/presentation-time/,presentation-time))
$(eval $(call generate_wayland,$(WL_PROTO_DIR)/stable/xdg-shell/,xdg-shell))
$(eval $(call generate_wayland,$(WL_PROTO_DIR)/unstable/xdg-decoration/,xdg-decoration-unstable-v1))
$(eval $(call generate_wayland,$(WL_PROTO_DIR)/stable/viewporter/,viewporter))
$(eval $(call generate_wayland,$(WL_PROTO_DIR)/unstable/linux-dmabuf/,linux-dmabuf-unstable-v1))
$(eval $(call generate_wayland,$(WL_PROTO_DIR)/staging/single-pixel-buffer/,single-pixel-buffer-v1))
$(eval $(call generate_wayland,$(WL_PROTO_DIR)/staging/fractional-scale/,fractional-scale-v1))
$(eval $(call generate_wayland,$(WL_PROTO_DIR)/staging/cursor-shape/,cursor-shape-v1))
$(eval $(call generate_wayland,$(WL_PROTO_DIR)/stable/tablet/,tablet-v2))
