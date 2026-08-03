CXX = g++
CXXFLAGS = -O3 -Wall -std=c++17
LDFLAGS = 

TARGET_DAEMON = ds4-translator
TARGET_CTL = ds4-ctl
TARGET_SPOOF = libudev-sony-spoof.so

DAEMON_SRC = src/main.cpp
CTL_SRC = src/ctl.cpp
SPOOF_SRC = src/udev-spoof.c

DAEMON_OBJ = src/main.o
CTL_OBJ = src/ctl.o

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
LIBDIR = $(PREFIX)/lib
SYSTEMDDIR = /etc/systemd/system

all: $(TARGET_DAEMON) $(TARGET_CTL) $(TARGET_SPOOF)

$(TARGET_DAEMON): $(DAEMON_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_CTL): $(CTL_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_SPOOF): $(SPOOF_SRC)
	gcc -O3 -fPIC -shared -o $@ $< -ldl

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(DAEMON_OBJ) $(CTL_OBJ) $(TARGET_DAEMON) $(TARGET_CTL) $(TARGET_SPOOF)

install: all
	install -D -m 755 $(TARGET_DAEMON) $(DESTDIR)$(BINDIR)/$(TARGET_DAEMON)
	install -D -m 755 $(TARGET_CTL) $(DESTDIR)$(BINDIR)/$(TARGET_CTL)
	install -D -m 755 $(TARGET_SPOOF) $(DESTDIR)$(LIBDIR)/$(TARGET_SPOOF)
	install -D -m 644 ds4-translator.service $(DESTDIR)$(SYSTEMDDIR)/ds4-translator.service
	install -D -m 644 72-ds4-translator-hide.rules $(DESTDIR)/etc/udev/rules.d/72-ds4-translator-hide.rules
	udevadm control --reload-rules
	udevadm trigger
	systemctl daemon-reload
	systemctl enable ds4-translator.service
	systemctl restart ds4-translator.service

uninstall:
	systemctl disable --now ds4-translator.service || true
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET_DAEMON)
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET_CTL)
	rm -f $(DESTDIR)$(LIBDIR)/$(TARGET_SPOOF)
	rm -f $(DESTDIR)$(SYSTEMDDIR)/ds4-translator.service
	rm -f $(DESTDIR)/etc/udev/rules.d/72-ds4-translator-hide.rules
	udevadm control --reload-rules
	udevadm trigger
	systemctl daemon-reload

.PHONY: all clean install uninstall
