#ifndef DS4_IPC_CLIENT_H
#define DS4_IPC_CLIENT_H

#include <cstdint>
#include <string>
#include <vector>

// Client side of the daemon's control protocol, shared by every program that
// talks to it (ds4-ctl and the Qt settings UI today).
//
// Both the wire transport and the command/argument rules live here on
// purpose: they are one protocol, and a second copy of either -- a socket
// path, an accepted backend name, the maximum controller-name length -- is a
// copy that drifts. Front-ends supply only their own I/O model: ds4-ctl calls
// the blocking send_command(), the UI drives connect_socket()/write_command()
// /read_available() from its event loop.
//
// Nothing here depends on Qt or on any UI toolkit.
namespace ds4ipc {

// Unix socket the daemon listens on (see the server side in src/main.cpp).
extern const char *const kSocketPath;

// Longest controller name the daemon accepts, in bytes.
constexpr int kMaxNameBytes = 63;

// Default round-trip budget. Generous because set-type/set-name/set-backend
// recreate the virtual device, which routinely takes seconds -- a shorter
// deadline reports a working daemon as a broken one.
constexpr int kDefaultTimeoutMs = 10000;

// ---------------------------------------------------------------- transport

// Opens a connection to the daemon. Returns the fd, or -1 with *error set.
// non_blocking leaves the fd in O_NONBLOCK for callers driving it from an
// event loop; the connect itself is still awaited, since a Unix-socket
// connect to a listening server completes immediately.
int connect_socket(std::string *error, bool non_blocking = false);

// Applies a send/receive deadline to a blocking fd.
void set_timeout(int fd, int timeout_ms);

// Switches an already connected fd to O_NONBLOCK. Front-ends with an event
// loop use this after writing the command, so only the read side is async.
bool set_non_blocking(int fd, std::string *error);

// Writes one command. Returns false with *error set on failure.
bool write_command(int fd, const std::string &command, std::string *error);

enum class ReadStatus {
    Data,  // *out got a chunk; more may follow
    Eof,   // daemon closed the connection -- the response is complete
    Again, // non-blocking fd with nothing to read yet
    Error, // *error is set
};

// Reads whatever is available. The daemon answers a command and closes, so
// Eof is the end-of-response marker, not a failure.
ReadStatus read_available(int fd, std::string *out, std::string *error);

// Removes the daemon's trailing newline(s) from a response.
void trim_response(std::string *response);

// Blocking single command/response exchange: connect, write, read to EOF,
// close. Returns false if the exchange itself failed (daemon unreachable,
// timed out, closed early), with the reason in out_response.
bool send_command(const std::string &command, std::string &out_response,
                  int timeout_ms = kDefaultTimeoutMs);

// ----------------------------------------------------------------- protocol

// Accepted argument values, in the order front-ends should offer them.
const std::vector<std::string> &controller_values();  // set-backend/set-name
const std::vector<std::string> &type_values();        // set-type
const std::vector<std::string> &backend_values();     // set-backend
const std::vector<std::string> &hide_method_values(); // set-hide-method

bool is_valid_controller(const std::string &value);
bool is_valid_type(const std::string &value);
bool is_valid_backend(const std::string &value);
bool is_valid_hide_method(const std::string &value);

// Joins a value list for use in error text ("ds4 dualsense").
std::string join_values(const std::vector<std::string> &values);

// Returns an empty string if the name is acceptable, otherwise the reason it
// is not, so a front-end can reject it before anything is sent.
std::string validate_name(const std::string &name);

// Command builders. Each returns the exact string to send, and sets *error
// (leaving the result empty) if an argument is not valid.
std::string build_set_type(const std::string &type, std::string *error);
std::string build_set_backend(const std::string &controller, const std::string &backend,
                              std::string *error);
// name == "--reset" restores the daemon's built-in default.
std::string build_set_name(const std::string &controller, const std::string &name,
                           std::string *error);
std::string build_set_hide_method(const std::string &method, std::string *error);

// Flashes the physical controller's light bar so the user can tell which
// device the daemon is actually holding.
extern const char *const kIdentifyCommand;

// Sets the physical controller's light bar and rumble motors directly, for
// testing them. Values are 0..255. A game (or the daemon's own passthrough)
// can overwrite this at any moment -- it is a test, not a setting.
std::string build_led(int red, int green, int blue, int rumble_left, int rumble_right,
                      std::string *error);
// Hands the light bar and motors back to whatever the emulated device asks
// for.
extern const char *const kLedResetCommand;

// Warning to show before a name change: it recreates the virtual device if
// that type is currently active.
extern const char *const kSetNameWarning;

// -------------------------------------------------------------- live input

// Commands that subscribe to the daemon's live input stream. Unlike every
// other command this connection stays open: the daemon promotes it into its
// broadcast subscriber list and pushes lines until the client disconnects.
//
// kTestCommand follows whichever source is active (the emulated device, or
// the physical controller when emulation is off). kTestAllCommand asks for
// both every tick, so the reader can show either one on demand.
extern const char *const kTestCommand;
extern const char *const kTestAllCommand;

// Values of InputState::source, and of the source word of a NOTE line.
extern const char *const kSourceVirtual;
extern const char *const kSourcePhysical;

// Button bits of a live STATE line. The daemon reports the raw HID button
// bytes; these are the decoded, front-end friendly bits, and they are also
// what `ds4-ctl virtual`'s synthetic "input" command takes.
enum Button : uint16_t {
    kBtnSquare   = 1 << 0,
    kBtnCross    = 1 << 1,
    kBtnCircle   = 1 << 2,
    kBtnTriangle = 1 << 3,
    kBtnL1       = 1 << 4,
    kBtnR1       = 1 << 5,
    kBtnL2       = 1 << 6,
    kBtnR2       = 1 << 7,
    kBtnShare    = 1 << 8,
    kBtnOptions  = 1 << 9,
    kBtnL3       = 1 << 10,
    kBtnR3       = 1 << 11,
    kBtnPs       = 1 << 12,
    kBtnTouchpad = 1 << 13,
};

// D-pad hat value: 0 = up, then clockwise in 45 degree steps, 8 = neutral.
constexpr uint8_t kDpadNeutral = 8;
const char *dpad_name(uint8_t dpad);

// Touchpad resolution the controller reports its contacts in.
constexpr int kTouchWidth = 1920;
constexpr int kTouchHeight = 943;

// One touchpad contact. The pad tracks two fingers at once, each with its
// own slot; a slot that is not being touched keeps the coordinates of the
// last contact, so `active` is what tells them apart.
struct TouchPoint {
    bool active = false;
    int x = 0; // 0..kTouchWidth-1, left to right
    int y = 0; // 0..kTouchHeight-1, top to bottom
};

// Decoded STATE line. Axes are the raw 0..255 HID values (128 is centre for
// the sticks, 0 is released for the triggers); motion is the raw signed
// sensor output, which is what the daemon forwards.
struct InputState {
    std::string source;      // "VIRTUAL" or "PHYSICAL"
    uint8_t lx = 128, ly = 128;
    uint8_t rx = 128, ry = 128;
    uint8_t l2 = 0, r2 = 0;
    uint16_t buttons = 0;    // OR of Button
    uint8_t dpad = kDpadNeutral;
    int16_t gyro[3] = {0, 0, 0};   // pitch, yaw, roll
    int16_t accel[3] = {0, 0, 0};  // x, y, z
    TouchPoint touch[2];
    // False when the line came from a daemon older than the motion (or the
    // touch) fields, so those values are zeros rather than readings.
    bool has_motion = false;
    bool has_touch = false;
};

// Builds one STATE line (daemon side), newline included. The daemon fills an
// InputState from the HID report it is relaying and hands it over here, so
// the wire format has exactly one writer.
std::string format_state_line(const std::string &source, const InputState &state);

// Decodes the button/d-pad bits out of the three raw HID button bytes.
void decode_buttons(const uint8_t bytes[3], uint16_t *out_buttons, uint8_t *out_dpad);

// Decodes one contact out of its four raw HID bytes (contact id/state byte,
// then the packed 12-bit x and y).
TouchPoint decode_touch_point(const uint8_t bytes[4]);

// Report timing of the physical controller over one measurement window, as
// the daemon measured it.
//
// The interval is the gap between two consecutive physical reports: its
// inverse is the rate the controller reports at, which is a property of the
// controller and its connection (USB and Bluetooth differ), not something the
// daemon picks. The latency is what the daemon adds on top: from having read
// a physical report to having written the emulated one.
struct TimingStats {
    uint32_t reports = 0;          // intervals measured in the window
    uint64_t window_us = 0;        // length of the window
    uint32_t interval_mean_us = 0;
    uint32_t interval_min_us = 0;
    uint32_t interval_max_us = 0;
    uint32_t latency_mean_us = 0;
    uint32_t latency_max_us = 0;

    // Reports per second over the window, 0 when nothing was measured.
    double reports_per_second() const;
};

// Builds one TIMING line (daemon side), newline included.
std::string format_timing_line(uint32_t reports, uint64_t window_us, uint32_t interval_mean_us,
                               uint32_t interval_min_us, uint32_t interval_max_us,
                               uint32_t latency_mean_us, uint32_t latency_max_us);

// Parses a TIMING line. Returns false for any other line.
bool parse_timing(const std::string &line, TimingStats *out);

// Parses a "NOTE <SOURCE> <text>" line: why that source has nothing to show
// right now. Returns false for any other line.
bool parse_note(const std::string &line, std::string *out_source, std::string *out_text);

// Parses one STATE line (front-end side). Returns false for any other line
// or for a malformed one, leaving *out untouched. A line from an older
// daemon (no motion or touch fields) still parses, with has_motion /
// has_touch false.
bool parse_state(const std::string &line, InputState *out);

// ------------------------------------------------------------------- status

// One "Key: Value" line of a `status` response, in the order the daemon
// printed it -- front-ends that just display the report need no key list.
struct StatusField {
    std::string key;
    std::string value;
};

std::vector<StatusField> parse_status(const std::string &response);

// Value for `key`, or an empty string if the response did not contain it.
std::string status_value(const std::vector<StatusField> &fields, const std::string &key);

// Maps the daemon's display name ("DualShock 4") to the config string its
// set-* commands take ("ds4"), and back.
std::string type_display_to_config(const std::string &display);
std::string type_config_to_display(const std::string &config);

} // namespace ds4ipc

#endif // DS4_IPC_CLIENT_H
