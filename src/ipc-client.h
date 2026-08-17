#ifndef DS4_IPC_CLIENT_H
#define DS4_IPC_CLIENT_H

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

// Warning to show before a name change: it recreates the virtual device if
// that type is currently active.
extern const char *const kSetNameWarning;

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
