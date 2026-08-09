# Prefer git's own description of the checked-out commit (exact tag, or
# <tag>-<n>-g<hash>[-dirty] between tags) since it's already the source of
# truth the release workflow tags from. Falls back to a VERSION file for
# builds without a .git directory — the release tarball ships one (written
# by the workflow from the tag it's building) so `make` still embeds a
# real version if a user extracts and rebuilds it themselves.
VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || cat VERSION 2>/dev/null || echo unknown)

CXX = g++
CC  = gcc

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

CXXFLAGS = -O3 $(STRICT_WARNINGS) -std=c++17 -DDS4_VERSION=\"$(VERSION)\" $(UNBIND_CXXFLAGS)
CFLAGS   = -O3 $(STRICT_WARNINGS) -DDS4_VERSION=\"$(VERSION)\" $(UNBIND_CXXFLAGS)
LDFLAGS  = -lpthread $(UNBIND_LDFLAGS)

TARGET_DAEMON = $(BUILD_DIR)/ds4-translator
TARGET_CTL    = $(BUILD_DIR)/ds4-ctl
TARGET_SPOOF  = $(BUILD_DIR)/libudev-sony-spoof.so
TARGET_SPOOF32 = $(BUILD_DIR)/libudev-sony-spoof32.so

DAEMON_SRC = src/main.cpp src/functionfs-backend.c $(UNBIND_SRC)
CTL_SRC    = src/ctl.cpp
SPOOF_SRC  = src/udev-spoof.c

DAEMON_OBJ = $(BUILD_DIR)/main.o $(BUILD_DIR)/functionfs-backend.o $(UNBIND_OBJ)
CTL_OBJ    = $(BUILD_DIR)/ctl.o

PREFIX    = /usr/local
BINDIR    = $(PREFIX)/bin
SYSTEMDDIR = /etc/systemd/system

all: $(TARGET_DAEMON) $(TARGET_CTL) $(TARGET_SPOOF) $(TARGET_SPOOF32)

# Debug build: no optimisation, debug symbols, DS4_DEBUG enabled
debug: CXXFLAGS = -O0 -g $(STRICT_WARNINGS) -std=c++17 -DDS4_DEBUG -DDS4_VERSION=\"$(VERSION)\" $(UNBIND_CXXFLAGS)
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

clean:
	rm -rf $(BUILD_DIR)

install: all
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
	udevadm control --reload-rules
	udevadm trigger
	systemctl daemon-reload
	systemctl enable ds4-translator.service
	systemctl restart ds4-translator.service
ifdef SUDO_USER
	chown -R $(SUDO_USER):$(SUDO_USER) $(BUILD_DIR)
endif

uninstall:
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
	udevadm control --reload-rules
	udevadm trigger
	systemctl daemon-reload

.PHONY: all debug clean install uninstall
