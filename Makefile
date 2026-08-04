CXX = g++
CC  = gcc
CXXFLAGS = -O3 -Wall -std=c++17
CFLAGS   = -O3 -Wall
LDFLAGS  = -lpthread

TARGET_DAEMON = ds4-translator
TARGET_CTL    = ds4-ctl
TARGET_SPOOF  = libudev-sony-spoof.so
TARGET_SPOOF32 = libudev-sony-spoof32.so
TARGET_INTERCEPT   = libds4-intercept.so
TARGET_INTERCEPT32 = libds4-intercept32.so

DAEMON_SRC = src/main.cpp src/raw-gadget-backend.c
CTL_SRC    = src/ctl.cpp
SPOOF_SRC  = src/udev-spoof.c
INTERCEPT_SRC = src/intercept.c

DAEMON_OBJ = src/main.o src/raw-gadget-backend.o
CTL_OBJ    = src/ctl.o

PREFIX    = /usr/local
BINDIR    = $(PREFIX)/bin
SYSTEMDDIR = /etc/systemd/system

all: $(TARGET_DAEMON) $(TARGET_CTL) $(TARGET_SPOOF) $(TARGET_SPOOF32)

# Debug build: no optimisation, debug symbols, DS4_DEBUG enabled
debug: CXXFLAGS = -O0 -g -Wall -std=c++17 -DDS4_DEBUG
debug: CFLAGS   = -O0 -g -Wall -DDS4_DEBUG
debug: clean all

$(TARGET_DAEMON): $(DAEMON_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_CTL): $(CTL_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_SPOOF): $(SPOOF_SRC)
	$(CC) -O3 -fPIC -shared -o $@ $< -ldl

$(TARGET_SPOOF32): $(SPOOF_SRC)
	$(CC) -m32 -O3 -fPIC -shared -o $@ $< -ldl

# Diagnostic intercept library (opt-in, not built by default)
intercept: $(TARGET_INTERCEPT) $(TARGET_INTERCEPT32)

$(TARGET_INTERCEPT): $(INTERCEPT_SRC)
	$(CC) -O3 -fPIC -shared -o $@ $< -ldl -lpthread

$(TARGET_INTERCEPT32): $(INTERCEPT_SRC)
	$(CC) -m32 -O3 -fPIC -shared -o $@ $< -ldl -lpthread

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@  $<

clean:
	rm -f $(DAEMON_OBJ) $(CTL_OBJ) $(TARGET_DAEMON) $(TARGET_CTL) $(TARGET_SPOOF) $(TARGET_SPOOF32) $(TARGET_INTERCEPT) $(TARGET_INTERCEPT32)

install: all
	install -D -m 755 $(TARGET_DAEMON) $(DESTDIR)$(BINDIR)/$(TARGET_DAEMON)
	install -D -m 755 $(TARGET_CTL) $(DESTDIR)$(BINDIR)/$(TARGET_CTL)
	install -D -m 755 $(TARGET_SPOOF) $(DESTDIR)/usr/lib/$(TARGET_SPOOF)
	install -D -m 755 $(TARGET_SPOOF32) $(DESTDIR)/usr/lib32/$(TARGET_SPOOF)
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

uninstall:
	systemctl disable --now ds4-translator.service || true
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET_DAEMON)
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET_CTL)
	rm -f $(DESTDIR)/usr/lib/$(TARGET_SPOOF)
	rm -f $(DESTDIR)/usr/lib32/$(TARGET_SPOOF)
	rm -f $(DESTDIR)$(SYSTEMDDIR)/ds4-translator.service
	rm -f $(DESTDIR)/etc/udev/rules.d/72-ds4-translator-hide.rules
	rm -f $(DESTDIR)/usr/share/man/man1/ds4-ctl.1
	rm -f $(DESTDIR)/usr/share/bash-completion/completions/ds4-ctl
	rm -f $(DESTDIR)/etc/modprobe.d/dummy_hcd.conf
	udevadm control --reload-rules
	udevadm trigger
	systemctl daemon-reload

.PHONY: all debug clean install uninstall intercept
