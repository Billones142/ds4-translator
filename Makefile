CXX = g++
CXXFLAGS = -O3 -Wall -std=c++17
LDFLAGS = 

TARGET_DAEMON = ds4-translator
TARGET_CTL = ds4-ctl

DAEMON_SRC = src/main.cpp
CTL_SRC = src/ctl.cpp

DAEMON_OBJ = src/main.o
CTL_OBJ = src/ctl.o

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
SYSTEMDDIR = /etc/systemd/system

all: $(TARGET_DAEMON) $(TARGET_CTL)

$(TARGET_DAEMON): $(DAEMON_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_CTL): $(CTL_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(DAEMON_OBJ) $(CTL_OBJ) $(TARGET_DAEMON) $(TARGET_CTL)

install: all
	install -D -m 755 $(TARGET_DAEMON) $(DESTDIR)$(BINDIR)/$(TARGET_DAEMON)
	install -D -m 755 $(TARGET_CTL) $(DESTDIR)$(BINDIR)/$(TARGET_CTL)
	install -D -m 644 ds4-translator.service $(DESTDIR)$(SYSTEMDDIR)/ds4-translator.service
	systemctl daemon-reload
	systemctl enable ds4-translator.service
	systemctl restart ds4-translator.service

uninstall:
	systemctl disable --now ds4-translator.service || true
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET_DAEMON)
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET_CTL)
	rm -f $(DESTDIR)$(SYSTEMDDIR)/ds4-translator.service
	systemctl daemon-reload

.PHONY: all clean install uninstall
