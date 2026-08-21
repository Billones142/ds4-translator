# Prefer git's own description of the checked-out commit (exact tag, or
# <tag>-<n>-g<hash>[-dirty] between tags) since it's already the source of
# truth the release workflow tags from. Falls back to a VERSION file for
# builds without a .git directory — the release tarball ships one (written
# by the workflow from the tag it's building) so `make` still embeds a
# real version if a user extracts and rebuilds it themselves.
VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || cat VERSION 2>/dev/null || echo unknown)

CXX = g++
CC  = gcc

PREFIX    = /usr/local
BINDIR    = $(PREFIX)/bin
SYSTEMDDIR = /etc/systemd/system

# This project talks to the kernel through raw ioctls, configfs, and manual
# struct-packed HID reports -- exactly the code where an unnoticed implicit
# conversion, shadowed variable, or format-string slip turns into a silent
# low-level bug (or a security issue) instead of a compile error. Errors,
# not just warnings: `make` refuses to produce a binary until these are
# clean, rather than letting them quietly ship.
# -Wno-missing-field-initializers: this codebase's designated-initializer
# structs (FunctionFSDevice) deliberately list only the fields
# that need a non-zero starting value -- the C/C++ standard guarantees
# every omitted field is zero/false/null-initialized, so warning on that is
# a false positive here, not a real omission.
STRICT_WARNINGS = -Wall -Wextra -Werror -Wshadow -Wformat=2 -Wformat-security \
                   -Wnull-dereference -Wpointer-arith -Wcast-align -Wundef -Wwrite-strings \
                   -Wno-missing-field-initializers

BUILD_DIR = build
UI_BUILD_DIR = ui/build
UI_BINARY    = $(UI_BUILD_DIR)/ds4-translator-ui

# Full HID-driver unbind + libusb raw-interrupt transport (see
# src/usb-hid-transport.* and src/hid-unbind-detect.*), the hide method
# that leaves no hidraw/input node for the physical controller at all
# (USB: fully readable via libusb; Bluetooth: driver unbind only, no live
# translation while hidden -- see set-hide-method). No longer gated behind
# a separate build flag: it's part of every build, same as the legacy
# chmod/setfacl/EVIOCGRAB method, and is the default hide_method.
ifeq ($(shell pkg-config --exists libusb-1.0 && echo yes),)
  $(error libusb-1.0 development package required (pkg-config libusb-1.0 not found))
endif
UNBIND_CXXFLAGS := $(shell pkg-config --cflags libusb-1.0)
UNBIND_LDFLAGS  := $(shell pkg-config --libs libusb-1.0)
UNBIND_SRC      := src/hid-unbind-detect.cpp src/usb-hid-transport.cpp
UNBIND_OBJ      := $(BUILD_DIR)/hid-unbind-detect.o $(BUILD_DIR)/usb-hid-transport.o

# HID-BPF replacement transport for the unbind hide method's Bluetooth
# side (see src/hid-bpf-transport.h and src/hid-bpf/hid-bpf-transport.bpf.c)
# -- USB's libusb transport above has no Bluetooth equivalent, so this
# fills that gap instead of leaving Bluetooth unbind-hidden-but-unreadable.
# Same dependency tier as libusb-1.0: hard build requirement, not optional.
ifeq ($(shell pkg-config --exists libbpf && echo yes),)
  $(error libbpf development package required (pkg-config libbpf not found))
endif
ifeq ($(shell command -v bpftool 2>/dev/null),)
  $(error bpftool not found (needed to generate vmlinux.h for the HID-BPF transport) -- try the "bpf" package)
endif
ifeq ($(shell command -v clang 2>/dev/null),)
  $(error clang not found (needed to compile src/hid-bpf/hid-bpf-transport.bpf.c))
endif
HIDBPF_CXXFLAGS := $(shell pkg-config --cflags libbpf)
HIDBPF_LDFLAGS  := $(shell pkg-config --libs libbpf)
HIDBPF_SRC      := src/hid-bpf-transport.cpp
HIDBPF_OBJ      := $(BUILD_DIR)/hid-bpf-transport.o
HIDBPF_BPF_OBJ  := $(BUILD_DIR)/hid-bpf-transport.bpf.o
HIDBPF_INSTALL_PATH := $(PREFIX)/lib/ds4-translator/hid-bpf-transport.bpf.o

CXXFLAGS = -O3 $(STRICT_WARNINGS) -std=c++17 -DDS4_VERSION=\"$(VERSION)\" \
           -DHID_BPF_OBJ_PATH=\"$(HIDBPF_INSTALL_PATH)\" $(UNBIND_CXXFLAGS) $(HIDBPF_CXXFLAGS)
CFLAGS   = -O3 $(STRICT_WARNINGS) -DDS4_VERSION=\"$(VERSION)\" $(UNBIND_CXXFLAGS)
LDFLAGS  = -lpthread $(UNBIND_LDFLAGS) $(HIDBPF_LDFLAGS)

TARGET_DAEMON = $(BUILD_DIR)/ds4-translator
TARGET_CTL    = $(BUILD_DIR)/ds4-ctl
TARGET_SPOOF  = $(BUILD_DIR)/libudev-sony-spoof.so
TARGET_SPOOF32 = $(BUILD_DIR)/libudev-sony-spoof32.so

DAEMON_SRC = src/main.cpp src/functionfs-backend.c $(UNBIND_SRC) $(IPC_SRC)
# The control protocol itself: the client transport/command rules used by
# ds4-ctl and the Qt UI in ui/ (which compiles src/ipc-client.cpp into its
# own binary), plus the live-monitor STATE line format, which the daemon
# writes and both front-ends parse -- so the daemon links it too.
IPC_SRC    = src/ipc-client.cpp
CTL_SRC    = src/ctl.cpp $(IPC_SRC)
SPOOF_SRC  = src/udev-spoof.c

DAEMON_OBJ = $(BUILD_DIR)/main.o $(BUILD_DIR)/functionfs-backend.o $(UNBIND_OBJ) $(HIDBPF_OBJ) \
             $(BUILD_DIR)/ipc-client.o
CTL_OBJ    = $(BUILD_DIR)/ctl.o $(BUILD_DIR)/ipc-client.o

all: $(TARGET_DAEMON) $(TARGET_CTL) $(TARGET_SPOOF) $(TARGET_SPOOF32) $(HIDBPF_BPF_OBJ)

# Debug build: no optimisation, debug symbols, DS4_DEBUG enabled
debug: CXXFLAGS = -O0 -g $(STRICT_WARNINGS) -std=c++17 -DDS4_DEBUG -DDS4_VERSION=\"$(VERSION)\" \
                  -DHID_BPF_OBJ_PATH=\"$(HIDBPF_INSTALL_PATH)\" $(UNBIND_CXXFLAGS) $(HIDBPF_CXXFLAGS)
debug: CFLAGS   = -O0 -g $(STRICT_WARNINGS) -DDS4_DEBUG -DDS4_VERSION=\"$(VERSION)\" $(UNBIND_CXXFLAGS)
debug: clean all

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET_DAEMON): $(DAEMON_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_CTL): $(CTL_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_SPOOF): $(SPOOF_SRC) | $(BUILD_DIR)
	$(CC) -O3 -fPIC -shared -o $@ $< -ldl

$(TARGET_SPOOF32): $(SPOOF_SRC) | $(BUILD_DIR)
	$(CC) -m32 -O3 -fPIC -shared -o $@ $< -ldl


$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Machine-generated (kernel BTF -> C types) for the HID-BPF transport's
# CO-RE relocations -- deliberately not tracked in git (huge, and only
# needed at build time; libbpf resolves actual field offsets against
# whatever kernel BTF is present on the machine that *loads* the
# program, not this one, so building here and running elsewhere is
# fine as long as that machine also exposes /sys/kernel/btf/vmlinux).
$(BUILD_DIR)/vmlinux.h: | $(BUILD_DIR)
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > $@

# clang (not $(CC)/$(CXX)) targeting the BPF backend directly -- this is
# not a native object file. -D__TARGET_ARCH_x86 hardcodes this project's
# only tested/supported arch, same as the rest of the build.
$(HIDBPF_BPF_OBJ): src/hid-bpf/hid-bpf-transport.bpf.c src/hid-bpf/hid_bpf.h \
                   src/hid-bpf/hid_bpf_helpers.h src/hid-bpf/hid_report_descriptor_helpers.h \
                   $(BUILD_DIR)/vmlinux.h | $(BUILD_DIR)
	clang -O2 -g -target bpf -D__TARGET_ARCH_x86 \
	    -I $(BUILD_DIR) -I src/hid-bpf -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(UI_BUILD_DIR)

# Qt 6 settings GUI (ui/). Deliberately kept out of `all`: it needs Qt 6 +
# CMake, which the daemon itself does not, so a plain `make` still builds the
# daemon on a machine without them. `install` does include it -- use
# `install-cli` there instead.
# Always delegated: CMake already does its own up-to-date checking, so
# there is nothing for make to track here.
#
# Except in an extracted release tarball, which ships both a VERSION file
# (written by the release workflow) and an already-built UI binary. Those
# two together mean "prebuilt release, not a source checkout", so the
# cmake/Qt step is skipped -- otherwise `make install` from a release
# would demand Qt 6 + CMake on a path whose whole point is needing no
# compiler. A real checkout has no VERSION file and always rebuilds.
ui:
	@if [ -f VERSION ] && [ -x $(UI_BINARY) ]; then \
	    echo "Using prebuilt $(UI_BINARY) from the release tarball"; \
	else \
	    cmake -S ui -B $(UI_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release && \
	    cmake --build $(UI_BUILD_DIR) -j$(shell nproc); \
	fi

# The .desktop file is not cosmetic: the desktop portal looks the app up by
# it, and without it every launch logs "App info not found for
# 'ds4-translator-ui'". No autostart entry is installed: the tray applet is
# off by default and the settings window writes ~/.config/autostart itself
# when the user turns "Start the applet on login" on.
install-ui: ui
	install -D -m 755 $(UI_BINARY) $(DESTDIR)$(BINDIR)/ds4-translator-ui
	install -D -m 644 ds4-translator-ui.desktop \
	    $(DESTDIR)/usr/share/applications/ds4-translator-ui.desktop
ifdef SUDO_USER
	chown -R $(SUDO_USER):$(SUDO_USER) $(UI_BUILD_DIR)
endif

uninstall-ui:
	rm -f $(DESTDIR)$(BINDIR)/ds4-translator-ui
	rm -f $(DESTDIR)/usr/share/applications/ds4-translator-ui.desktop
	# Older versions installed this; removed here so an upgrade+uninstall
	# does not leave a system-wide autostart entry behind.
	rm -f $(DESTDIR)/etc/xdg/autostart/ds4-translator-applet.desktop

# Same thing for the current user only -- no root needed.
USER_BINDIR      = $(HOME)/.local/bin
USER_APPDIR      = $(HOME)/.local/share/applications
USER_AUTOSTART   = $(HOME)/.config/autostart

install-ui-user: ui
	install -D -m 755 $(UI_BINARY) $(USER_BINDIR)/ds4-translator-ui
	install -D -m 644 ds4-translator-ui.desktop \
	    $(USER_APPDIR)/ds4-translator-ui.desktop
	update-desktop-database $(USER_APPDIR) 2>/dev/null || true

uninstall-ui-user:
	rm -f $(USER_BINDIR)/ds4-translator-ui
	rm -f $(USER_APPDIR)/ds4-translator-ui.desktop
	rm -f $(USER_AUTOSTART)/ds4-translator-applet.desktop
	update-desktop-database $(USER_APPDIR) 2>/dev/null || true

# Everything: daemon, CLI and the Qt settings UI. `install-cli` below is the
# same thing without the UI, for machines with no Qt 6 / CMake (the UI is the
# only part that needs them) or where a GUI has no place at all.
install: install-cli install-ui

install-cli: all
	install -D -m 755 $(TARGET_DAEMON) $(DESTDIR)$(BINDIR)/$(notdir $(TARGET_DAEMON))
	install -D -m 755 $(TARGET_CTL) $(DESTDIR)$(BINDIR)/$(notdir $(TARGET_CTL))
	install -D -m 755 $(TARGET_SPOOF) $(DESTDIR)/usr/lib/$(notdir $(TARGET_SPOOF))
	install -D -m 755 $(TARGET_SPOOF32) $(DESTDIR)/usr/lib32/$(notdir $(TARGET_SPOOF32))
	install -D -m 755 rebind-unbound-controllers.sh $(DESTDIR)$(BINDIR)/rebind-unbound-controllers.sh
	install -D -m 644 ds4-translator.service $(DESTDIR)$(SYSTEMDDIR)/ds4-translator.service
	install -D -m 644 72-ds4-translator-hide.rules $(DESTDIR)/etc/udev/rules.d/72-ds4-translator-hide.rules
	install -D -m 644 ds4-ctl.1 $(DESTDIR)/usr/share/man/man1/ds4-ctl.1
	install -D -m 644 ds4-ctl-completion.bash $(DESTDIR)/usr/share/bash-completion/completions/ds4-ctl
	install -D -m 644 dummy_hcd.conf $(DESTDIR)/etc/modprobe.d/dummy_hcd.conf
	install -D -m 644 $(HIDBPF_BPF_OBJ) $(DESTDIR)$(HIDBPF_INSTALL_PATH)
	udevadm control --reload-rules
	udevadm trigger
	systemctl daemon-reload
	systemctl enable ds4-translator.service
	systemctl restart ds4-translator.service
ifdef SUDO_USER
	chown -R $(SUDO_USER):$(SUDO_USER) $(BUILD_DIR)
endif

# Mirrors install: removes the UI too. uninstall-cli leaves it in place.
uninstall: uninstall-ui uninstall-cli

uninstall-cli:
	systemctl disable --now ds4-translator.service || true
	rm -f $(DESTDIR)$(BINDIR)/$(notdir $(TARGET_DAEMON))
	rm -f $(DESTDIR)$(BINDIR)/$(notdir $(TARGET_CTL))
	rm -f $(DESTDIR)/usr/lib/$(notdir $(TARGET_SPOOF))
	rm -f $(DESTDIR)/usr/lib32/$(notdir $(TARGET_SPOOF32))
	rm -f $(DESTDIR)$(BINDIR)/rebind-unbound-controllers.sh
	rm -f $(DESTDIR)$(SYSTEMDDIR)/ds4-translator.service
	rm -f $(DESTDIR)/etc/udev/rules.d/72-ds4-translator-hide.rules
	rm -f $(DESTDIR)/usr/share/man/man1/ds4-ctl.1
	rm -f $(DESTDIR)/usr/share/bash-completion/completions/ds4-ctl
	rm -f $(DESTDIR)/etc/modprobe.d/dummy_hcd.conf
	rm -f $(DESTDIR)$(HIDBPF_INSTALL_PATH)
	udevadm control --reload-rules
	udevadm trigger
	systemctl daemon-reload

.PHONY: all debug clean install install-cli uninstall uninstall-cli \
        ui install-ui uninstall-ui install-ui-user uninstall-ui-user
