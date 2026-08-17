#include "ipc-client.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace ds4ipc {

const char *const kSocketPath = "/run/ds4-translator.sock";

const char *const kSetNameWarning =
    "changing the controller name recreates the virtual device if that type is "
    "currently active -- any app/game reading it will see a brief input interruption.";

namespace {

std::string errno_text() {
    return std::string(strerror(errno));
}

bool contains(const std::vector<std::string> &values, const std::string &value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

// Every command is a single short line, so one write is enough in practice;
// a partial write is still retried rather than silently truncating.
bool write_all(int fd, const char *data, size_t size, std::string *error) {
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(fd, data + written, size - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (error) {
                *error = "Failed to write to daemon: " + errno_text();
            }
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

int connect_socket(std::string *error, bool non_blocking) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error) {
            *error = "Failed to create socket: " + errno_text();
        }
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        if (error) {
            *error = "Failed to connect to translation daemon (is the ds4-translator "
                     "service running?): " +
                     errno_text();
        }
        close(fd);
        return -1;
    }

    // Set after connect(): a Unix-socket connect to a listening server
    // completes immediately, so there is no partial-connect state to handle,
    // and callers only need the non-blocking mode for the exchange itself.
    if (non_blocking && !set_non_blocking(fd, error)) {
        close(fd);
        return -1;
    }

    return fd;
}

bool set_non_blocking(int fd, std::string *error) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        if (error) {
            *error = "Failed to set the socket non-blocking: " + errno_text();
        }
        return false;
    }
    return true;
}

void set_timeout(int fd, int timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = static_cast<suseconds_t>(timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

bool write_command(int fd, const std::string &command, std::string *error) {
    return write_all(fd, command.c_str(), command.size(), error);
}

ReadStatus read_available(int fd, std::string *out, std::string *error) {
    char buf[1024];
    ssize_t n;
    do {
        n = read(fd, buf, sizeof(buf));
    } while (n < 0 && errno == EINTR);

    if (n > 0) {
        if (out) {
            out->append(buf, static_cast<size_t>(n));
        }
        return ReadStatus::Data;
    }
    if (n == 0) {
        return ReadStatus::Eof;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Nothing readable yet on a non-blocking fd; on a blocking one this
        // is SO_RCVTIMEO expiring.
        return ReadStatus::Again;
    }
    if (error) {
        *error = "Failed to read from daemon: " + errno_text();
    }
    return ReadStatus::Error;
}

void trim_response(std::string *response) {
    if (!response) {
        return;
    }
    while (!response->empty() && (response->back() == '\n' || response->back() == '\r')) {
        response->pop_back();
    }
}

bool send_command(const std::string &command, std::string &out_response, int timeout_ms) {
    std::string error;
    int fd = connect_socket(&error, false);
    if (fd < 0) {
        out_response = error;
        return false;
    }
    set_timeout(fd, timeout_ms);

    if (!write_command(fd, command, &error)) {
        out_response = error;
        close(fd);
        return false;
    }

    std::string response;
    for (;;) {
        ReadStatus status = read_available(fd, &response, &error);
        if (status == ReadStatus::Eof) {
            break;
        }
        if (status == ReadStatus::Error) {
            // A blocking fd only reports EAGAIN once SO_RCVTIMEO expires.
            out_response = error;
            close(fd);
            return false;
        }
        if (status == ReadStatus::Again) {
            break; // blocking fd: SO_RCVTIMEO expired
        }
        // ReadStatus::Data: the daemon closes right after the response, so
        // keep reading until EOF rather than assuming one read is the whole
        // message.
    }
    close(fd);

    if (response.empty()) {
        out_response = "Error: No response from daemon (connection timed out or closed).";
        return false;
    }

    trim_response(&response);
    out_response = response;
    return true;
}

const std::vector<std::string> &controller_values() {
    static const std::vector<std::string> values = {"ds4", "dualsense"};
    return values;
}

const std::vector<std::string> &type_values() {
    static const std::vector<std::string> values = {"ds4", "dualsense", "none", "hidden"};
    return values;
}

const std::vector<std::string> &backend_values() {
    static const std::vector<std::string> values = {"uhid", "functionfs"};
    return values;
}

const std::vector<std::string> &hide_method_values() {
    static const std::vector<std::string> values = {"legacy", "unbind"};
    return values;
}

bool is_valid_controller(const std::string &value) {
    return contains(controller_values(), value);
}

bool is_valid_type(const std::string &value) {
    return contains(type_values(), value);
}

bool is_valid_backend(const std::string &value) {
    return contains(backend_values(), value);
}

bool is_valid_hide_method(const std::string &value) {
    return contains(hide_method_values(), value);
}

std::string join_values(const std::vector<std::string> &values) {
    std::string joined;
    for (const std::string &value : values) {
        if (!joined.empty()) {
            joined += " ";
        }
        joined += value;
    }
    return joined;
}

std::string validate_name(const std::string &name) {
    if (name.empty()) {
        return "name cannot be empty (use --reset to restore the default).";
    }
    if (name.find('\n') != std::string::npos || name.find('\r') != std::string::npos) {
        return "name cannot contain newline characters.";
    }
    if (name.size() > static_cast<size_t>(kMaxNameBytes)) {
        return "name too long (" + std::to_string(name.size()) + " bytes, max " +
               std::to_string(kMaxNameBytes) + ").";
    }
    return std::string();
}

namespace {

// Shared shape of every builder's argument check: on failure the caller gets
// the same "Invalid X. Supported: ..." text no matter which front-end asked.
bool check_value(const std::string &value, const std::vector<std::string> &allowed,
                 const char *what, std::string *error) {
    if (contains(allowed, value)) {
        return true;
    }
    if (error) {
        *error = "Invalid " + std::string(what) + " '" + value +
                 "'. Supported: " + join_values(allowed);
    }
    return false;
}

} // namespace

std::string build_set_type(const std::string &type, std::string *error) {
    if (!check_value(type, type_values(), "type", error)) {
        return std::string();
    }
    return "set-type " + type;
}

std::string build_set_backend(const std::string &controller, const std::string &backend,
                              std::string *error) {
    if (!check_value(controller, controller_values(), "controller type", error) ||
        !check_value(backend, backend_values(), "backend", error)) {
        return std::string();
    }
    return "set-backend " + controller + " " + backend;
}

std::string build_set_name(const std::string &controller, const std::string &name,
                           std::string *error) {
    if (!check_value(controller, controller_values(), "controller type", error)) {
        return std::string();
    }
    if (name != "--reset") {
        const std::string problem = validate_name(name);
        if (!problem.empty()) {
            if (error) {
                *error = problem;
            }
            return std::string();
        }
    }
    return "set-name " + controller + " " + name;
}

std::string build_set_hide_method(const std::string &method, std::string *error) {
    if (!check_value(method, hide_method_values(), "hide method", error)) {
        return std::string();
    }
    return "set-hide-method " + method;
}

std::vector<StatusField> parse_status(const std::string &response) {
    std::vector<StatusField> fields;
    size_t start = 0;
    while (start <= response.size()) {
        size_t end = response.find('\n', start);
        if (end == std::string::npos) {
            end = response.size();
        }
        const std::string line = response.substr(start, end - start);
        start = end + 1;

        const size_t sep = line.find(':');
        if (sep == std::string::npos) {
            continue;
        }
        StatusField field;
        field.key = line.substr(0, sep);
        field.value = line.substr(sep + 1);
        // Keys never have leading space; values always have exactly one.
        while (!field.value.empty() && (field.value.front() == ' ')) {
            field.value.erase(field.value.begin());
        }
        while (!field.value.empty() && (field.value.back() == ' ' || field.value.back() == '\r')) {
            field.value.pop_back();
        }
        fields.push_back(field);
    }
    return fields;
}

std::string status_value(const std::vector<StatusField> &fields, const std::string &key) {
    for (const StatusField &field : fields) {
        if (field.key == key) {
            return field.value;
        }
    }
    return std::string();
}

std::string type_display_to_config(const std::string &display) {
    if (display == "DualShock 4") return "ds4";
    if (display == "DualSense") return "dualsense";
    if (display == "Hidden") return "hidden";
    return "none";
}

} // namespace ds4ipc
