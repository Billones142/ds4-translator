#include <iostream>
#include <string>
#include <vector>
#include <deque>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <unistd.h>
#include <termios.h>
#include <poll.h>

#include "ipc-client.h"

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
    std::cout << "  set-name <ds4|dualsense> <name>  Override the controller name/product string reported to" << std::endl;
    std::cout << "                                   the OS (recreates the device if that type is currently" << std::endl;
    std::cout << "                                   active -- brief input interruption -- otherwise just" << std::endl;
    std::cout << "                                   persists for later). Max 63 bytes, no newlines." << std::endl;
    std::cout << "  set-name <ds4|dualsense> --reset Restore the default Sony name for that controller type" << std::endl;
    std::cout << "                                   NOTE: vendor ID stays Sony's regardless -- some games/" << std::endl;
    std::cout << "                                   drivers cross-check name against vendor ID, so a custom" << std::endl;
    std::cout << "                                   name may raise compatibility suspicion in those cases." << std::endl;
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
    std::cout << "  set-hide-method <legacy|unbind>  Change how the physical controller is hidden from other" << std::endl;
    std::cout << "                                   apps (applies on the next physical (re)connection). unbind" << std::endl;
    std::cout << "                                   (default) hides it before any other app can see it, with" << std::endl;
    std::cout << "                                   live translation either way: USB fully unbinds the kernel" << std::endl;
    std::cout << "                                   HID driver (no hidraw/input node exists at all) and reads" << std::endl;
    std::cout << "                                   it via libusb instead; Bluetooth keeps the driver bound" << std::endl;
    std::cout << "                                   (hidraw/input nodes still exist) but uses a HID-BPF" << std::endl;
    std::cout << "                                   program to make every report invisible to them while" << std::endl;
    std::cout << "                                   mirroring it to this daemon. legacy is the older" << std::endl;
    std::cout << "                                   chmod/setfacl/EVIOCGRAB method -- node exists but" << std::endl;
    std::cout << "                                   permission-blocked, for both USB and Bluetooth." << std::endl;
}

// One command/response exchange, using the shared client in ipc-client.cpp
// so ds4-ctl and the settings UI speak to the daemon through exactly the same
// code path.
static bool send_command(const std::string& cmd, std::string& out_response) {
    return ds4ipc::send_command(cmd, out_response);
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

// Same bits the daemon's live STATE line decodes to, so the synthetic
// tester below and the live monitor speak one vocabulary.
enum {
    SYN_BTN_SQUARE   = ds4ipc::kBtnSquare,
    SYN_BTN_CROSS    = ds4ipc::kBtnCross,
    SYN_BTN_CIRCLE   = ds4ipc::kBtnCircle,
    SYN_BTN_TRIANGLE = ds4ipc::kBtnTriangle,
    SYN_BTN_L1       = ds4ipc::kBtnL1,
    SYN_BTN_R1       = ds4ipc::kBtnR1,
    SYN_BTN_L2       = ds4ipc::kBtnL2,
    SYN_BTN_R2       = ds4ipc::kBtnR2,
    SYN_BTN_SHARE    = ds4ipc::kBtnShare,
    SYN_BTN_OPTIONS  = ds4ipc::kBtnOptions,
    SYN_BTN_L3       = ds4ipc::kBtnL3,
    SYN_BTN_R3       = ds4ipc::kBtnR3,
    SYN_BTN_PS       = ds4ipc::kBtnPs,
    SYN_BTN_TOUCHPAD = ds4ipc::kBtnTouchpad,
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
    {"set-name",         nullptr}, // position-aware (ds4|dualsense, then free-text name), special-cased below
    {"create-virtual",   "ds4 dualsense"},
    {"destroy-virtual",  nullptr},
    {"virtual",          "ds4 dualsense --auto"},
    {"release-physical", nullptr},
    {"resume-physical",  nullptr},
    {"tap",              nullptr},
    {"test",             nullptr},
    {"set-hide-method",  "legacy unbind"},
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
    (void)snprintf(cmdbuf, sizeof(cmdbuf), "input %x %u %u %u %u %u %u %u",
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
    std::cout << "\x1b[0KD-Pad (arrow keys): " << ds4ipc::dpad_name(s.dpad) << "\n";
    std::cout << "\x1b[0K\n";
    std::cout << "\x1b[0K   " << mark(s.buttons, SYN_BTN_L1, "L1(Q)") << "  " << mark(s.buttons, SYN_BTN_R1, "R1(E)")
              << "      " << mark(s.buttons, SYN_BTN_L2, "L2(1)") << "  " << mark(s.buttons, SYN_BTN_R2, "R2(2)")
              << "      " << mark(s.buttons, SYN_BTN_L3, "L3(Z)") << "  " << mark(s.buttons, SYN_BTN_R3, "R3(X)") << "\n";
    std::cout << "\x1b[0K   " << mark(s.buttons, SYN_BTN_SHARE, "Share(-)") << "  " << mark(s.buttons, SYN_BTN_OPTIONS, "Options(=)")
              << "      " << mark(s.buttons, SYN_BTN_PS, "PS(P)") << "  " << mark(s.buttons, SYN_BTN_TOUCHPAD, "Touchpad(T)") << "\n";
    std::cout << "\x1b[0K\n";
    char hexbuf[16];
    (void)snprintf(hexbuf, sizeof(hexbuf), "0x%04X", s.buttons);
    std::cout << "\x1b[0KRaw button bitmask: " << hexbuf << "   dpad=" << (int)s.dpad << "\n";
    std::cout << "\x1b[0K(Sticks fixed centered; L2/R2 analog mirrors digital press)\n";
    std::cout << "\x1b[0K\n";
    std::cout << "\x1b[0K'r' = reset all buttons\n";
    std::cout << "\x1b[0K\n";
    std::cout << "\x1b[0KDaemon status:\n";
    if (status_ok) {
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

struct TestState {
    bool have_state = false;
    ds4ipc::InputState in;
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
        std::cout << "\x1b[0KSource: " << st.in.source << "\n";
        std::cout << "\x1b[0K\n";
        std::cout << "\x1b[0K              " << mark(st.in.buttons, SYN_BTN_TRIANGLE, "Triangle") << "\n";
        std::cout << "\x1b[0K   " << mark(st.in.buttons, SYN_BTN_SQUARE, "Square") << "                " << mark(st.in.buttons, SYN_BTN_CIRCLE, "Circle") << "\n";
        std::cout << "\x1b[0K              " << mark(st.in.buttons, SYN_BTN_CROSS, "Cross") << "\n";
        std::cout << "\x1b[0K\n";
        std::cout << "\x1b[0KD-Pad: " << ds4ipc::dpad_name(st.in.dpad) << "\n";
        std::cout << "\x1b[0K   " << mark(st.in.buttons, SYN_BTN_L1, "L1") << "  " << mark(st.in.buttons, SYN_BTN_R1, "R1")
                  << "      " << mark(st.in.buttons, SYN_BTN_L2, "L2") << "  " << mark(st.in.buttons, SYN_BTN_R2, "R2")
                  << "      " << mark(st.in.buttons, SYN_BTN_L3, "L3") << "  " << mark(st.in.buttons, SYN_BTN_R3, "R3") << "\n";
        std::cout << "\x1b[0K   " << mark(st.in.buttons, SYN_BTN_SHARE, "Share") << "  " << mark(st.in.buttons, SYN_BTN_OPTIONS, "Options")
                  << "      " << mark(st.in.buttons, SYN_BTN_PS, "PS") << "  " << mark(st.in.buttons, SYN_BTN_TOUCHPAD, "Touchpad") << "\n";
        std::cout << "\x1b[0K\n";
        char buf[160];
        (void)snprintf(buf, sizeof(buf), "LX=%3u LY=%3u   RX=%3u RY=%3u   L2=%3u R2=%3u",
                 st.in.lx, st.in.ly, st.in.rx, st.in.ry, st.in.l2, st.in.r2);
        std::cout << "\x1b[0K" << buf << "\n";
        // Raw sensor counts, as the controller reports them -- no scaling to
        // degrees/s or g, which would need per-unit calibration data.
        (void)snprintf(buf, sizeof(buf), "Gyro  P=%6d Y=%6d R=%6d   Accel X=%6d Y=%6d Z=%6d",
                 st.in.gyro[0], st.in.gyro[1], st.in.gyro[2],
                 st.in.accel[0], st.in.accel[1], st.in.accel[2]);
        std::cout << "\x1b[0K" << buf << "\n";
        // The touchpad tracks two fingers; a slot keeps its last coordinates
        // after the finger leaves, so only the active ones are worth showing.
        std::string touch_text;
        for (int i = 0; i < 2; ++i) {
            const ds4ipc::TouchPoint& point = st.in.touch[i];
            (void)snprintf(buf, sizeof(buf), "  #%d %s", i + 1,
                     point.active ? "" : "-");
            touch_text += buf;
            if (point.active) {
                (void)snprintf(buf, sizeof(buf), "%4d,%3d", point.x, point.y);
                touch_text += buf;
            }
        }
        std::cout << "\x1b[0KTouchpad (max " << ds4ipc::kTouchWidth << "x" << ds4ipc::kTouchHeight
                  << "):" << touch_text << "\n";
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
    // "test" is the one command whose connection stays open: the daemon
    // promotes this socket into its broadcast subscriber list, so the
    // exchange is driven here rather than through send_command().
    std::string error;
    int fd = ds4ipc::connect_socket(&error);
    if (fd < 0) {
        std::cerr << error << std::endl;
        print_daemon_unreachable_help();
        return 1;
    }

    if (!ds4ipc::write_command(fd, ds4ipc::kTestCommand, &error)) {
        std::cerr << error << std::endl;
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
            std::string read_error;
            ds4ipc::ReadStatus status = ds4ipc::read_available(fd, &rx_accum, &read_error);
            if (status == ds4ipc::ReadStatus::Eof || status == ds4ipc::ReadStatus::Error) {
                std::cout << "\nDaemon connection closed." << std::endl;
                break;
            }
            if (status == ds4ipc::ReadStatus::Data) {
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
                        ds4ipc::InputState parsed;
                        if (ds4ipc::parse_state(line, &parsed)) {
                            st.have_state = true;
                            st.in = parsed;
                        }
                    } else if (line.rfind("NOTE ", 0) == 0) {
                        st.have_state = false;
                        st.note = line.substr(5);
                    } else if (line.rfind("EVENT ", 0) == 0) {
                        time_t t = time(nullptr);
                        struct tm tmv;
                        localtime_r(&t, &tmv);
                        char ts[16];
                        (void)strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
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
        } else if (target == "set-name") {
            // Same shape as set-backend for the first argument; the second
            // is a free-text name, so there's nothing useful to complete.
            if (argc <= 3) {
                std::cout << "ds4 dualsense" << std::endl;
            } else if (argc == 4) {
                std::cout << "--reset" << std::endl;
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

    if (cmd == "set-name") {
        // Free-text name argument (may contain spaces), unlike every other
        // command here -- handled as its own early-return branch rather than
        // joining the generic full_cmd path below, which assumes single-
        // token arguments.
        if (argc < 4) {
            std::cerr << "Error: set-name requires <ds4|dualsense> <name> (or --reset)" << std::endl;
            return 1;
        }
        std::string ctrl = argv[2];
        std::string name_arg;
        if (std::string(argv[3]) == "--reset") {
            name_arg = "--reset";
        } else {
            for (int i = 3; i < argc; ++i) {
                if (i > 3) name_arg += " ";
                name_arg += argv[i];
            }
        }
        std::string build_error;
        std::string command = ds4ipc::build_set_name(ctrl, name_arg, &build_error);
        if (command.empty()) {
            std::cerr << "Error: " << build_error << std::endl;
            return 1;
        }
        std::cerr << "Warning: " << ds4ipc::kSetNameWarning << std::endl;
        std::string response;
        if (!send_command(command, response)) {
            std::cerr << response << std::endl;
            print_daemon_unreachable_help();
            return 1;
        }
        std::cout << response << std::endl;
        return 0;
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

    // The set-* commands build their command string through ds4ipc so their
    // accepted values are defined once, for every front-end. create-virtual
    // keeps the spec table: it shares set-type's argument name but not its
    // value list (no none/hidden).
    std::string full_cmd = cmd;
    std::string build_error;
    if (cmd == "set-backend") {
        if (argc < 4) {
            std::cerr << "Error: set-backend requires <ds4|dualsense> <uhid|functionfs>" << std::endl;
            return 1;
        }
        full_cmd = ds4ipc::build_set_backend(argv[2], argv[3], &build_error);
    } else if (cmd == "set-type") {
        if (argc < 3) {
            std::cerr << "Error: set-type requires a target type ("
                      << ds4ipc::join_values(ds4ipc::type_values()) << ")" << std::endl;
            return 1;
        }
        full_cmd = ds4ipc::build_set_type(argv[2], &build_error);
    } else if (cmd == "set-hide-method") {
        if (argc < 3) {
            std::cerr << "Error: set-hide-method requires "
                      << ds4ipc::join_values(ds4ipc::hide_method_values()) << std::endl;
            return 1;
        }
        full_cmd = ds4ipc::build_set_hide_method(argv[2], &build_error);
    } else if (cmd == "create-virtual") {
        if (argc < 3) {
            std::cerr << "Error: create-virtual requires a target type (" << spec->arg_completions
                      << ")" << std::endl;
            return 1;
        }
        std::string type = argv[2];
        if (!is_in_list(type, spec->arg_completions)) {
            std::cerr << "Error: Invalid type. Supported: " << spec->arg_completions << std::endl;
            return 1;
        }
        full_cmd += " " + type;
    }
    if (full_cmd.empty()) {
        std::cerr << "Error: " << build_error << std::endl;
        return 1;
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
