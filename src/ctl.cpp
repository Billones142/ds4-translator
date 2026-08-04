#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

void print_usage() {
    std::cout << "Usage: ds4-ctl <command> [args]" << std::endl;
    std::cout << "Commands:" << std::endl;
    std::cout << "  status                   Get current status of the translation daemon" << std::endl;
    std::cout << "  set-type <ds4|dualsense|none> Change the virtual controller emulation type at runtime" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd = argv[1];
    std::string full_cmd = cmd;
    if (cmd == "set-type") {
        if (argc < 3) {
            std::cerr << "Error: set-type requires a target type (ds4, dualsense, or none)" << std::endl;
            return 1;
        }
        std::string type = argv[2];
        if (type != "ds4" && type != "dualsense" && type != "none") {
            std::cerr << "Error: Invalid type. Supported: ds4, dualsense, none" << std::endl;
            return 1;
        }
        full_cmd += " " + type;
    } else if (cmd == "status") {
        // ok
    } else {
        std::cerr << "Error: Unknown command '" << cmd << "'" << std::endl;
        print_usage();
        return 1;
    }

    // Connect to Unix Domain Socket
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        return 1;
    }

    // Set 3 seconds timeout for send/receive to prevent hanging
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
        std::cerr << "Failed to connect to translation daemon (is the ds4-translator service running?): " 
                  << strerror(errno) << std::endl;
        std::cerr << "\nTroubleshooting Commands:" << std::endl;
        std::cerr << "  1. Check translator service status:" << std::endl;
        std::cerr << "     systemctl status ds4-translator.service" << std::endl;
        std::cerr << "  2. View recent logs from the daemon:" << std::endl;
        std::cerr << "     journalctl -u ds4-translator.service -n 20" << std::endl;
        std::cerr << "  3. Verify kernel modules are loaded:" << std::endl;
        std::cerr << "     lsmod | grep -E \"raw_gadget|dummy_hcd\"" << std::endl;
        close(fd);
        return 1;
    }

    // Write command
    if (write(fd, full_cmd.c_str(), full_cmd.size()) < 0) {
        std::cerr << "Failed to write to daemon: " << strerror(errno) << std::endl;
        close(fd);
        return 1;
    }

    // Read response
    char rx_buf[1024];
    memset(rx_buf, 0, sizeof(rx_buf));
    ssize_t bytes_read = read(fd, rx_buf, sizeof(rx_buf) - 1);
    if (bytes_read > 0) {
        std::cout << rx_buf << std::endl;
    } else {
        std::cerr << "Error: No response from daemon (connection timed out or closed)." << std::endl;
        std::cerr << "\nTroubleshooting Commands:" << std::endl;
        std::cerr << "  1. Check if the daemon is active or stuck:" << std::endl;
        std::cerr << "     systemctl status ds4-translator.service" << std::endl;
        std::cerr << "  2. Inspect the latest kernel logs for resets:" << std::endl;
        std::cerr << "     journalctl -k -n 30" << std::endl;
    }

    close(fd);
    return 0;
}
