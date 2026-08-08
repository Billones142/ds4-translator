#include <iostream>
#include <string>
#include <vector>
#include <deque>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <termios.h>
#include <poll.h>

static std::string button_names_joined();

#ifndef DS4_VERSION
#define DS4_VERSION "unknown"
#endif

void print_usage() {
    std::cout << "Usage: ds4-ctl <command> [args]" << std::endl;
    std::cout << "Commands:" << std::endl;
    std::cout << "  version                          Print ds4-ctl's version" << std::endl;
    std::cout << "  status                          Get current status of the translation daemon" << std::endl;
    std::cout << "  set-type <ds4|dualsense|none|hidden>   Change the virtual controller emulation type at runtime" << std::endl;
    std::cout << "                                   (none = fully untouched physical passthrough, hidden = physical" << std::endl;
    std::cout << "                                   hidden from other apps but not translated)" << std::endl;
    std::cout << "  set-backend <ds4|dualsense> <uhid|functionfs>   Change that controller type's backend at" << std::endl;
    std::cout << "                                   runtime (recreates the device if that type is currently" << std::endl;
    std::cout << "                                   active, otherwise just persists for later). Defaults:" << std::endl;
    std::cout << "                                   dualsense=uhid, ds4=functionfs -- uhid never reliably" << std::endl;
    std::cout << "                                   enumerates as a real USB device, which some Wine/Proton" << std::endl;
    std::cout << "                                   titles need to detect DS4 emulation; functionfs does." << std::endl;
    std::cout << "  create-virtual <ds4|dualsense>   Create a standalone virtual controller, no physical pad needed" << std::endl;
    std::cout << "  destroy-virtual                  Destroy the standalone virtual controller" << std::endl;
    std::cout << "  virtual [ds4|dualsense] [--auto] Create (if needed) a standalone virtual controller and open" << std::endl;
    std::cout << "                                   an interactive keyboard button-tester UI to drive it" << std::endl;
    std::cout << "                                   (--auto: destroy it on exit, but only if this run is the" << std::endl;
    std::cout << "                                   one that created it)" << std::endl;
    std::cout << "  release-physical                 Stop using/re-scanning the physical controller (leaves it" << std::endl;
    std::cout << "                                   plugged in but untouched) so create-virtual can run instead" << std::endl;
    std::cout << "  resume-physical                  Re-enable physical controller auto-scan after release-physical" << std::endl;
    std::cout << "  tap <button>                     Briefly press+release one button on the active standalone" << std::endl;
    std::cout << "                                   virtual controller. For scripting automated input patterns." << std::endl;
    std::cout << "                                   Buttons: " << button_names_joined() << std::endl;
    std::cout << "  test                             Live-monitor the active controller: current button/stick" << std::endl;
    std::cout << "                                   state in real time, plus a scrolling log of LED/rumble" << std::endl;
    std::cout << "                                   commands sent to it. Shows the virtual device for ds4/" << std::endl;
    std::cout << "                                   dualsense types, or the raw physical controller for" << std::endl;
    std::cout << "                                   none/hidden (LED/rumble aren't observable for none)." << std::endl;
}

// Connect to the daemon's Unix socket, send one command, return its response.
// Returns false if the connection/send/recv itself failed (daemon unreachable).
static bool send_command(const std::string& cmd, std::string& out_response) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        out_response = std::string("Failed to create socket: ") + strerror(errno);
        return false;
    }

    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/run/ds4-translator.sock", sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        out_response = std::string("Failed to connect to translation daemon (is the ds4-translator service running?): ") + strerror(errno);
        close(fd);
        return false;
    }

    if (write(fd, cmd.c_str(), cmd.size()) < 0) {
        out_response = std::string("Failed to write to daemon: ") + strerror(errno);
        close(fd);
        return false;
    }

    char rx_buf[1024];
    memset(rx_buf, 0, sizeof(rx_buf));
    ssize_t bytes_read = read(fd, rx_buf, sizeof(rx_buf) - 1);
    close(fd);

    if (bytes_read <= 0) {
        out_response = "Error: No response from daemon (connection timed out or closed).";
        return false;
    }

    out_response = rx_buf;
    while (!out_response.empty() && (out_response.back() == '\n' || out_response.back() == '\r')) {
        out_response.pop_back();
    }
    return true;
}

static void print_daemon_unreachable_help() {
    std::cerr << "\nTroubleshooting Commands:" << std::endl;
    std::cerr << "  1. Check translator service status:" << std::endl;
    std::cerr << "     systemctl status ds4-translator.service" << std::endl;
    std::cerr << "  2. View recent logs from the daemon:" << std::endl;
    std::cerr << "     journalctl -u ds4-translator.service -n 20" << std::endl;
    std::cerr << "  3. Verify kernel modules are loaded:" << std::endl;
    std::cerr << "     lsmod | grep -E \"dummy_hcd|libcomposite|usb_f_fs\"" << std::endl;
}

// ---------- Interactive virtual controller button tester ----------
//
// Every key below is a *toggle*, not a hold: terminals don't deliver
// key-release events, so there is no way to know when a key is physically
// let go. Press once to set a button, press again to clear it. This is
// enough to test controller *detection* (does the game/Wine see a DS4 and
// react to button state changes at all), not to actually play with a
// keyboard. Analog sticks are intentionally left fixed at center (128,128)
// rather than half-implemented with awkward toggle semantics.

enum {
    SYN_BTN_SQUARE   = 1 << 0,
    SYN_BTN_CROSS    = 1 << 1,
    SYN_BTN_CIRCLE   = 1 << 2,
    SYN_BTN_TRIANGLE = 1 << 3,
    SYN_BTN_L1       = 1 << 4,
    SYN_BTN_R1       = 1 << 5,
    SYN_BTN_L2       = 1 << 6,
    SYN_BTN_R2       = 1 << 7,
    SYN_BTN_SHARE    = 1 << 8,
    SYN_BTN_OPTIONS  = 1 << 9,
    SYN_BTN_L3       = 1 << 10,
    SYN_BTN_R3       = 1 << 11,
    SYN_BTN_PS       = 1 << 12,
    SYN_BTN_TOUCHPAD = 1 << 13,
};

// Single source of truth for `tap`'s button/dpad names, shared by run_tap(),
// usage text, and `--complete-args tap` (queried live by
// ds4-ctl-completion.bash) so a new button here shows up in completion
// automatically instead of needing the bash script hand-edited too.
struct ButtonSpec {
    const char *name;
    uint16_t bit;
    uint8_t dpad;
};
static const ButtonSpec kButtons[] = {
    {"square",   SYN_BTN_SQUARE,   8},
    {"cross",    SYN_BTN_CROSS,    8},
    {"circle",   SYN_BTN_CIRCLE,   8},
    {"triangle", SYN_BTN_TRIANGLE, 8},
    {"l1",       SYN_BTN_L1,       8},
    {"r1",       SYN_BTN_R1,       8},
    {"l2",       SYN_BTN_L2,       8},
    {"r2",       SYN_BTN_R2,       8},
    {"l3",       SYN_BTN_L3,       8},
    {"r3",       SYN_BTN_R3,       8},
    {"share",    SYN_BTN_SHARE,    8},
    {"options",  SYN_BTN_OPTIONS,  8},
    {"ps",       SYN_BTN_PS,       8},
    {"touchpad", SYN_BTN_TOUCHPAD, 8},
    {"up",       0,                0},
    {"down",     0,                4},
    {"left",     0,                6},
    {"right",    0,                2},
};

static std::string button_names_joined() {
    std::string out;
    for (const auto& b : kButtons) {
        if (!out.empty()) out += " ";
        out += b.name;
    }
    return out;
}

// Single source of truth for top-level commands and their valid next
// argument, shared by main()'s dispatch/validation and `--commands`/
// `--complete-args` (queried live by ds4-ctl-completion.bash). `tap`'s
// completions come from kButtons instead, since they're a longer, separate
// list (see --complete-args handling in main()).
struct CommandSpec {
    const char *name;
    const char *arg_completions; // space-separated tokens, or nullptr
};
static const CommandSpec kCommands[] = {
    {"version",          nullptr},
    {"status",           nullptr},
    {"set-type",         "ds4 dualsense none hidden"},
    {"set-backend",      nullptr}, // position-aware, special-cased in --complete-args below
    {"create-virtual",   "ds4 dualsense"},
    {"destroy-virtual",  nullptr},
    {"virtual",          "ds4 dualsense --auto"},
    {"release-physical", nullptr},
    {"resume-physical",  nullptr},
    {"tap",              nullptr},
    {"test",             nullptr},
};

static const CommandSpec* find_command(const std::string& name) {
    for (const auto& c : kCommands) {
        if (name == c.name) return &c;
    }
    return nullptr;
}

static bool is_in_list(const std::string& word, const char *space_separated) {
    std::string s(space_separated);
    size_t start = 0;
    while (start < s.size()) {
        size_t sp = s.find(' ', start);
        if (sp == std::string::npos) sp = s.size();
        if (s.compare(start, sp - start, word) == 0) return true;
        start = sp + 1;
    }
    return false;
}

struct UiState {
    uint16_t buttons = 0;
    uint8_t dpad = 8; // 0=up..7=up-left clockwise, 8=neutral
};

static const char *dpad_name(uint8_t dpad) {
    switch (dpad) {
        case 0: return "Up";
        case 1: return "Up-Right";
        case 2: return "Right";
        case 3: return "Down-Right";
        case 4: return "Down";
        case 5: return "Down-Left";
        case 6: return "Left";
        case 7: return "Up-Left";
        default: return "Neutral";
    }
}

static std::string mark(uint16_t buttons, uint16_t bit, const char *label) {
    return (buttons & bit) ? (std::string("[") + label + "]") : (std::string(" ") + label + " ");
}

static void send_ui_state(const UiState& s) {
    char cmdbuf[128];
    // buttons(hex) dpad lx ly rx ry l2 r2 — sticks fixed centered, L2/R2
    // analog mirrors the digital press so trigger-threshold checks still see
    // a plausible value.
    uint8_t l2 = (s.buttons & SYN_BTN_L2) ? 255 : 0;
    uint8_t r2 = (s.buttons & SYN_BTN_R2) ? 255 : 0;
    snprintf(cmdbuf, sizeof(cmdbuf), "input %x %u %u %u %u %u %u %u",
             s.buttons, s.dpad, 128, 128, 128, 128, l2, r2);
    std::string resp;
    send_command(cmdbuf, resp);
}

static void redraw(const UiState& s, const std::string& daemon_status, bool status_ok) {
    // Full redraw each frame; the terminal is cleared once at UI start, so
    // this only repositions the cursor rather than clearing (avoids flicker).
    std::cout << "\x1b[H";
    std::cout << "\x1b[0KDS4-CTL Virtual Controller Tester        (Esc / Ctrl+C to quit)\n";
    std::cout << "\x1b[0K\n";
    std::cout << "\x1b[0K              " << mark(s.buttons, SYN_BTN_TRIANGLE, "Triangle(I)") << "\n";
    std::cout << "\x1b[0K   " << mark(s.buttons, SYN_BTN_SQUARE, "Square(J)") << "                      " << mark(s.buttons, SYN_BTN_CIRCLE, "Circle(L)") << "\n";
    std::cout << "\x1b[0K              " << mark(s.buttons, SYN_BTN_CROSS, "Cross(K)") << "\n";
    std::cout << "\x1b[0K\n";
    std::cout << "\x1b[0KD-Pad (arrow keys): " << dpad_name(s.dpad) << "\n";
    std::cout << "\x1b[0K\n";
    std::cout << "\x1b[0K   " << mark(s.buttons, SYN_BTN_L1, "L1(Q)") << "  " << mark(s.buttons, SYN_BTN_R1, "R1(E)")
              << "      " << mark(s.buttons, SYN_BTN_L2, "L2(1)") << "  " << mark(s.buttons, SYN_BTN_R2, "R2(2)")
              << "      " << mark(s.buttons, SYN_BTN_L3, "L3(Z)") << "  " << mark(s.buttons, SYN_BTN_R3, "R3(X)") << "\n";
    std::cout << "\x1b[0K   " << mark(s.buttons, SYN_BTN_SHARE, "Share(-)") << "  " << mark(s.buttons, SYN_BTN_OPTIONS, "Options(=)")
              << "      " << mark(s.buttons, SYN_BTN_PS, "PS(P)") << "  " << mark(s.buttons, SYN_BTN_TOUCHPAD, "Touchpad(T)") << "\n";
    std::cout << "\x1b[0K\n";
    char hexbuf[16];
    snprintf(hexbuf, sizeof(hexbuf), "0x%04X", s.buttons);
    std::cout << "\x1b[0KRaw button bitmask: " << hexbuf << "   dpad=" << (int)s.dpad << "\n";
    std::cout << "\x1b[0K(Sticks fixed centered; L2/R2 analog mirrors digital press)\n";
    std::cout << "\x1b[0K\n";
    std::cout << "\x1b[0K'r' = reset all buttons\n";
    std::cout << "\x1b[0K\n";
    std::cout << "\x1b[0KDaemon status:\n";
    if (status_ok) {
        std::string line;
        size_t start = 0;
        while (start < daemon_status.size()) {
            size_t nl = daemon_status.find('\n', start);
            if (nl == std::string::npos) nl = daemon_status.size();
            std::cout << "\x1b[0K  " << daemon_status.substr(start, nl - start) << "\n";
            start = nl + 1;
        }
    } else {
        std::cout << "\x1b[0K  (unreachable: " << daemon_status << ")\n";
    }
    std::cout << "\x1b[0J" << std::flush;
}

static int run_virtual_ui(const std::string& type, bool auto_destroy) {
    std::string resp;
    if (!send_command("create-virtual " + type, resp)) {
        std::cerr << resp << std::endl;
        print_daemon_unreachable_help();
        return 1;
    }
    if (resp.rfind("Error", 0) == 0) {
        std::cerr << resp << std::endl;
        return 1;
    }
    // Only auto-destroy on exit if this invocation is the one that actually
    // created the device (response starts "OK:") -- if one was already
    // active ("Virtual controller already active."), it likely belongs to
    // some other session/purpose, and --auto shouldn't silently rip that
    // away just because this run also happened to want one.
    bool created_by_us = resp.rfind("OK:", 0) == 0;
    std::cout << resp << std::endl;
    if (auto_destroy) {
        std::cout << "Starting interactive tester... (--auto: virtual controller will be destroyed on exit)" << std::endl;
    } else {
        std::cout << "Starting interactive tester... (leaves the virtual controller running on exit;\n"
                     "use 'ds4-ctl destroy-virtual' to remove it)" << std::endl;
    }
    sleep(1);

    struct termios orig_term, raw_term;
    if (tcgetattr(STDIN_FILENO, &orig_term) < 0) {
        std::cerr << "Failed to query terminal settings: " << strerror(errno) << std::endl;
        return 1;
    }
    raw_term = orig_term;
    // ISIG is cleared too (not just ICANON/ECHO): otherwise a real Ctrl+C
    // raises SIGINT and the terminal driver never delivers byte 0x03 to
    // read() at all, so the loop's own "c == 3" Ctrl+C handling below never
    // runs and the process dies via the default SIGINT disposition instead
    // -- skipping tcsetattr restoration and, now, the --auto destroy-on-exit
    // below. Clearing ISIG makes Ctrl+C arrive as plain data like every
    // other key here, so it always goes through the normal exit path.
    raw_term.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw_term.c_cc[VMIN] = 0;
    raw_term.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw_term);

    std::cout << "\x1b[2J\x1b[H" << std::flush; // clear once; redraw() repositions afterward

    UiState state;
    bool quit = false;
    std::string last_status = "";
    bool last_status_ok = false;
    int status_refresh_counter = 0;

    send_ui_state(state);
    last_status_ok = send_command("status", last_status);
    redraw(state, last_status, last_status_ok);

    while (!quit) {
        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 150);

        bool changed = false;

        if (pr > 0 && (pfd.revents & POLLIN)) {
            unsigned char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 0x1b) {
                    // Could be a bare Esc, or the start of an arrow-key
                    // escape sequence (ESC [ A/B/C/D). Peek briefly.
                    struct pollfd pfd2;
                    pfd2.fd = STDIN_FILENO;
                    pfd2.events = POLLIN;
                    if (poll(&pfd2, 1, 20) > 0) {
                        unsigned char seq[2] = {0, 0};
                        ssize_t n = read(STDIN_FILENO, seq, 2);
                        if (n == 2 && seq[0] == '[') {
                            uint8_t new_dpad = state.dpad;
                            switch (seq[1]) {
                                case 'A': new_dpad = (state.dpad == 0) ? 8 : 0; break; // Up
                                case 'B': new_dpad = (state.dpad == 4) ? 8 : 4; break; // Down
                                case 'C': new_dpad = (state.dpad == 2) ? 8 : 2; break; // Right
                                case 'D': new_dpad = (state.dpad == 6) ? 8 : 6; break; // Left
                                default: break;
                            }
                            if (new_dpad != state.dpad) {
                                state.dpad = new_dpad;
                                changed = true;
                            }
                        }
                    } else {
                        quit = true; // bare Esc
                    }
                } else if (c == 3) { // Ctrl+C
                    quit = true;
                } else {
                    uint16_t bit = 0;
                    switch (c) {
                        case 'i': case 'I': bit = SYN_BTN_TRIANGLE; break;
                        case 'j': case 'J': bit = SYN_BTN_SQUARE;   break;
                        case 'k': case 'K': bit = SYN_BTN_CROSS;    break;
                        case 'l': case 'L': bit = SYN_BTN_CIRCLE;   break;
                        case 'q': case 'Q': bit = SYN_BTN_L1;       break;
                        case 'e': case 'E': bit = SYN_BTN_R1;       break;
                        case '1':           bit = SYN_BTN_L2;       break;
                        case '2':           bit = SYN_BTN_R2;       break;
                        case 'z': case 'Z': bit = SYN_BTN_L3;       break;
                        case 'x': case 'X': bit = SYN_BTN_R3;       break;
                        case '-':           bit = SYN_BTN_SHARE;    break;
                        case '=':           bit = SYN_BTN_OPTIONS;  break;
                        case 'p': case 'P': bit = SYN_BTN_PS;       break;
                        case 't': case 'T': bit = SYN_BTN_TOUCHPAD; break;
                        case 'r': case 'R':
                            state.buttons = 0;
                            state.dpad = 8;
                            changed = true;
                            break;
                        default: break;
                    }
                    if (bit) {
                        state.buttons ^= bit;
                        changed = true;
                    }
                }
            }
        }

        if (changed) {
            send_ui_state(state);
        }

        // Refresh daemon status roughly every ~1.5s (150ms poll * 10)
        if (changed || ++status_refresh_counter >= 10) {
            status_refresh_counter = 0;
            last_status_ok = send_command("status", last_status);
            redraw(state, last_status, last_status_ok);
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
    if (auto_destroy && created_by_us) {
        std::string destroy_resp;
        send_command("destroy-virtual", destroy_resp);
        std::cout << "\nExiting tester. " << destroy_resp << std::endl;
    } else if (auto_destroy) {
        std::cout << "\nExiting tester. --auto was set, but this session didn't create the virtual controller "
                     "(one was already active); leaving it running. Use 'ds4-ctl destroy-virtual' to remove it." << std::endl;
    } else {
        std::cout << "\nExiting tester. Virtual controller left running; use 'ds4-ctl destroy-virtual' to remove it." << std::endl;
    }
    return 0;
}

// ---------- Live monitor (`ds4-ctl test`) ----------
//
// Read-only counterpart to the button tester above: instead of driving
// input, it subscribes to the daemon's "test" stream (a persistent
// connection the daemon keeps open and pushes lines to, rather than the
// one-shot request/response every other command uses — see the "test"
// handler in main.cpp) and displays whatever the daemon says the active
// controller — virtual device, or raw physical passthrough for type=none/
// hidden — currently looks like, plus a scrolling log of LED/rumble
// output commands it observed being sent to it.

static UiState decode_raw_state(uint8_t b0, uint8_t b1, uint8_t b2) {
    UiState s;
    s.dpad = b0 & 0x0F;
    if (b0 & 0x10) s.buttons |= SYN_BTN_SQUARE;
    if (b0 & 0x20) s.buttons |= SYN_BTN_CROSS;
    if (b0 & 0x40) s.buttons |= SYN_BTN_CIRCLE;
    if (b0 & 0x80) s.buttons |= SYN_BTN_TRIANGLE;
    if (b1 & 0x01) s.buttons |= SYN_BTN_L1;
    if (b1 & 0x02) s.buttons |= SYN_BTN_R1;
    if (b1 & 0x04) s.buttons |= SYN_BTN_L2;
    if (b1 & 0x08) s.buttons |= SYN_BTN_R2;
    if (b1 & 0x10) s.buttons |= SYN_BTN_SHARE;
    if (b1 & 0x20) s.buttons |= SYN_BTN_OPTIONS;
    if (b1 & 0x40) s.buttons |= SYN_BTN_L3;
    if (b1 & 0x80) s.buttons |= SYN_BTN_R3;
    if (b2 & 0x01) s.buttons |= SYN_BTN_PS;
    if (b2 & 0x02) s.buttons |= SYN_BTN_TOUCHPAD;
    return s;
}

struct TestState {
    bool have_state = false;
    std::string source;
    UiState ui;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
    std::string note = "Waiting for data from daemon...";
};

static void redraw_test(const std::string& header, const std::vector<std::string>& caveats,
                         const TestState& st, const std::deque<std::string>& events) {
    std::cout << "\x1b[H";
    std::cout << "\x1b[0KDS4-CTL Live Monitor                     (Esc / q / Ctrl+C to quit)\n";
    std::cout << "\x1b[0K" << header << "\n";
    for (const auto& c : caveats) {
        std::cout << "\x1b[0K  ! " << c << "\n";
    }
    std::cout << "\x1b[0K\n";
    if (!st.have_state) {
        std::cout << "\x1b[0K" << st.note << "\n";
    } else {
        std::cout << "\x1b[0KSource: " << st.source << "\n";
        std::cout << "\x1b[0K\n";
        std::cout << "\x1b[0K              " << mark(st.ui.buttons, SYN_BTN_TRIANGLE, "Triangle") << "\n";
        std::cout << "\x1b[0K   " << mark(st.ui.buttons, SYN_BTN_SQUARE, "Square") << "                " << mark(st.ui.buttons, SYN_BTN_CIRCLE, "Circle") << "\n";
        std::cout << "\x1b[0K              " << mark(st.ui.buttons, SYN_BTN_CROSS, "Cross") << "\n";
        std::cout << "\x1b[0K\n";
        std::cout << "\x1b[0KD-Pad: " << dpad_name(st.ui.dpad) << "\n";
        std::cout << "\x1b[0K   " << mark(st.ui.buttons, SYN_BTN_L1, "L1") << "  " << mark(st.ui.buttons, SYN_BTN_R1, "R1")
                  << "      " << mark(st.ui.buttons, SYN_BTN_L2, "L2") << "  " << mark(st.ui.buttons, SYN_BTN_R2, "R2")
                  << "      " << mark(st.ui.buttons, SYN_BTN_L3, "L3") << "  " << mark(st.ui.buttons, SYN_BTN_R3, "R3") << "\n";
        std::cout << "\x1b[0K   " << mark(st.ui.buttons, SYN_BTN_SHARE, "Share") << "  " << mark(st.ui.buttons, SYN_BTN_OPTIONS, "Options")
                  << "      " << mark(st.ui.buttons, SYN_BTN_PS, "PS") << "  " << mark(st.ui.buttons, SYN_BTN_TOUCHPAD, "Touchpad") << "\n";
        std::cout << "\x1b[0K\n";
        char buf[128];
        snprintf(buf, sizeof(buf), "LX=%3u LY=%3u   RX=%3u RY=%3u   L2=%3u R2=%3u",
                 st.lx, st.ly, st.rx, st.ry, st.l2, st.r2);
        std::cout << "\x1b[0K" << buf << "\n";
    }
    std::cout << "\x1b[0K\n";
    std::cout << "\x1b[0KLED/rumble commands received, most recent last:\n";
    if (events.empty()) {
        std::cout << "\x1b[0K  (none yet)\n";
    } else {
        for (const auto& e : events) {
            std::cout << "\x1b[0K  " << e << "\n";
        }
    }
    std::cout << "\x1b[0J" << std::flush;
}

static int run_test_ui() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/run/ds4-translator.sock", sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to connect to translation daemon (is the ds4-translator service running?): " << strerror(errno) << std::endl;
        close(fd);
        print_daemon_unreachable_help();
        return 1;
    }

    std::string req = "test";
    if (write(fd, req.c_str(), req.size()) < 0) {
        std::cerr << "Failed to write to daemon: " << strerror(errno) << std::endl;
        close(fd);
        return 1;
    }

    struct termios orig_term, raw_term;
    if (tcgetattr(STDIN_FILENO, &orig_term) < 0) {
        std::cerr << "Failed to query terminal settings: " << strerror(errno) << std::endl;
        close(fd);
        return 1;
    }
    raw_term = orig_term;
    raw_term.c_lflag &= ~(ICANON | ECHO);
    raw_term.c_cc[VMIN] = 0;
    raw_term.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw_term);

    std::cout << "\x1b[2J\x1b[H" << std::flush;

    std::string header = "(connecting...)";
    std::vector<std::string> caveats;
    TestState st;
    std::deque<std::string> events;
    const size_t MAX_EVENTS = 12;

    std::string rx_accum;
    bool quit = false;
    bool need_redraw = true;

    while (!quit) {
        struct pollfd pfds[2];
        pfds[0].fd = fd;
        pfds[0].events = POLLIN;
        pfds[1].fd = STDIN_FILENO;
        pfds[1].events = POLLIN;
        int pr = poll(pfds, 2, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (pr > 0 && (pfds[0].revents & (POLLHUP | POLLERR))) {
            std::cout << "\nDaemon connection closed." << std::endl;
            break;
        }

        if (pr > 0 && (pfds[0].revents & POLLIN)) {
            char buf[1024];
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) {
                if (!(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
                    std::cout << "\nDaemon connection closed." << std::endl;
                    break;
                }
            } else {
                rx_accum.append(buf, (size_t)n);
                size_t nl;
                while ((nl = rx_accum.find('\n')) != std::string::npos) {
                    std::string line = rx_accum.substr(0, nl);
                    rx_accum.erase(0, nl + 1);
                    need_redraw = true;

                    if (line.rfind("OK:", 0) == 0) {
                        header = line;
                    } else if (line.rfind("CAVEAT ", 0) == 0) {
                        caveats.push_back(line.substr(7));
                    } else if (line.rfind("STATE ", 0) == 0) {
                        char source[16];
                        unsigned x, y, rxv, ry, z, rz, b0, b1, b2;
                        int nf = sscanf(line.c_str(), "STATE %15s %u %u %u %u %u %u %x %x %x",
                                        source, &x, &y, &rxv, &ry, &z, &rz, &b0, &b1, &b2);
                        if (nf == 10) {
                            st.have_state = true;
                            st.source = source;
                            st.lx = (uint8_t)x;  st.ly = (uint8_t)y;
                            st.rx = (uint8_t)rxv; st.ry = (uint8_t)ry;
                            st.l2 = (uint8_t)z;  st.r2 = (uint8_t)rz;
                            st.ui = decode_raw_state((uint8_t)b0, (uint8_t)b1, (uint8_t)b2);
                        }
                    } else if (line.rfind("NOTE ", 0) == 0) {
                        st.have_state = false;
                        st.note = line.substr(5);
                    } else if (line.rfind("EVENT ", 0) == 0) {
                        time_t t = time(nullptr);
                        struct tm tmv;
                        localtime_r(&t, &tmv);
                        char ts[16];
                        strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
                        events.push_back(std::string("[") + ts + "] " + line.substr(6));
                        if (events.size() > MAX_EVENTS) events.pop_front();
                    }
                }
            }
        }

        if (pr > 0 && (pfds[1].revents & POLLIN)) {
            unsigned char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 0x1b || c == 3 || c == 'q' || c == 'Q') {
                    quit = true;
                }
            }
        }

        if (need_redraw) {
            need_redraw = false;
            redraw_test(header, caveats, st, events);
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
    close(fd);
    std::cout << "\nExiting live monitor." << std::endl;
    return 0;
}

// One-shot press+release for scripting automated input patterns (e.g. the
// wine-controller-probe harness driving synthetic button presses while a
// probe polls the device, with no human at the keyboard). Sends the button
// pressed, holds briefly, then sends fully-neutral state.
static int run_tap(const std::string& button) {
    const ButtonSpec *spec = nullptr;
    for (const auto& b : kButtons) {
        if (button == b.name) { spec = &b; break; }
    }
    if (!spec) {
        std::cerr << "Error: unknown button '" << button << "'. Supported: "
                   << button_names_joined() << std::endl;
        return 1;
    }

    UiState s;
    s.buttons = spec->bit;
    s.dpad = spec->dpad;
    send_ui_state(s);
    usleep(120000);
    UiState released;
    send_ui_state(released);
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "version" || cmd == "--version" || cmd == "-v") {
        std::cout << "ds4-ctl " << DS4_VERSION << std::endl;
        return 0;
    }

    // Hidden, machine-readable queries used by ds4-ctl-completion.bash. The
    // bash script asks the binary for its command list / per-command
    // argument list at completion time instead of hardcoding them, so
    // adding a command to kCommands (or a button to kButtons) is all that's
    // needed to keep tab-completion in sync — no separate file to edit.
    if (cmd == "--commands") {
        for (const auto& c : kCommands) std::cout << c.name << "\n";
        return 0;
    }
    if (cmd == "--complete-args") {
        if (argc < 3) return 0;
        std::string target = argv[2];
        if (target == "tap") {
            std::cout << button_names_joined() << std::endl;
        } else if (target == "set-backend") {
            // Position-aware (unlike every other command here): argv[3], if
            // present, is the controller word already typed -- offer
            // ds4/dualsense first, then uhid/functionfs once one of those is
            // picked. argv[3:] wasn't passed at all until
            // ds4-ctl-completion.bash started forwarding every prior word
            // instead of just the command name.
            if (argc <= 3) {
                std::cout << "ds4 dualsense" << std::endl;
            } else if (argc == 4) {
                std::cout << "uhid functionfs" << std::endl;
            }
        } else if (const CommandSpec *spec = find_command(target)) {
            if (spec->arg_completions) std::cout << spec->arg_completions << std::endl;
        }
        return 0;
    }

    const CommandSpec *spec = find_command(cmd);
    if (!spec) {
        std::cerr << "Error: Unknown command '" << cmd << "'" << std::endl;
        print_usage();
        return 1;
    }

    if (cmd == "virtual") {
        std::string type = "ds4";
        bool auto_destroy = false;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--auto") {
                auto_destroy = true;
            } else if (is_in_list(arg, spec->arg_completions)) {
                type = arg;
            } else {
                std::cerr << "Error: Invalid argument '" << arg << "'. Supported: " << spec->arg_completions << " --auto" << std::endl;
                return 1;
            }
        }
        return run_virtual_ui(type, auto_destroy);
    }

    if (cmd == "tap") {
        if (argc < 3) {
            std::cerr << "Error: tap requires a button name (see 'ds4-ctl' usage)" << std::endl;
            return 1;
        }
        return run_tap(argv[2]);
    }

    if (cmd == "test") {
        return run_test_ui();
    }

    std::string full_cmd = cmd;
    if (cmd == "set-backend") {
        if (argc < 4) {
            std::cerr << "Error: set-backend requires <ds4|dualsense> <uhid|functionfs>" << std::endl;
            return 1;
        }
        std::string ctrl = argv[2];
        std::string backend = argv[3];
        if (ctrl != "ds4" && ctrl != "dualsense") {
            std::cerr << "Error: Invalid controller type. Supported: ds4 dualsense" << std::endl;
            return 1;
        }
        if (backend != "uhid" && backend != "functionfs") {
            std::cerr << "Error: Invalid backend. Supported: uhid functionfs" << std::endl;
            return 1;
        }
        full_cmd += " " + ctrl + " " + backend;
    } else if (cmd == "set-type" || cmd == "create-virtual") {
        if (argc < 3) {
            std::cerr << "Error: " << cmd << " requires a target type (" << spec->arg_completions << ")" << std::endl;
            return 1;
        }
        std::string type = argv[2];
        if (!is_in_list(type, spec->arg_completions)) {
            std::cerr << "Error: Invalid type. Supported: " << spec->arg_completions << std::endl;
            return 1;
        }
        full_cmd += " " + type;
    }
    // status/destroy-virtual/release-physical/resume-physical take no args;
    // full_cmd is already just the command name.

    std::string response;
    if (!send_command(full_cmd, response)) {
        std::cerr << response << std::endl;
        print_daemon_unreachable_help();
        return 1;
    }
    std::cout << response << std::endl;
    return 0;
}
