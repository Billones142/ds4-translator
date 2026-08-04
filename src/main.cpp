#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <linux/uhid.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "descriptors.h"

namespace fs = std::filesystem;

enum ControllerType {
    TYPE_DS4,
    TYPE_DUALSENSE,
    TYPE_NONE
};

struct PhysicalNode {
    std::string path;
    int fd;
    mode_t orig_mode;
    bool is_grabbed;
};

std::atomic<bool> running(true);

void signal_handler(int sig) {
    running = false;
}

// CRC32 calculation matching Linux kernel (IEEE 802.3 Ethernet polynomial 0xEDB88320)
uint32_t calculate_crc32(uint8_t seed, const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool table_initialized = false;
    if (!table_initialized) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) {
                if (c & 1) {
                    c = 0xEDB88320L ^ (c >> 1);
                } else {
                    c = c >> 1;
                }
            }
            table[i] = c;
        }
        table_initialized = true;
    }

    uint32_t crc = 0xFFFFFFFF;
    crc = table[(crc ^ seed) & 0xFF] ^ (crc >> 8);
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

// Structures matching hid-playstation.c layout
struct __attribute__((packed)) dualshock4_input_report_common {
    uint8_t x, y;
    uint8_t rx, ry;
    uint8_t buttons[3];
    uint8_t z, rz;
    uint16_t sensor_timestamp;
    uint8_t sensor_temperature;
    int16_t gyro[3];
    int16_t accel[3];
    uint8_t reserved2[5];
    uint8_t status[2];
    uint8_t reserved3;
};

struct __attribute__((packed)) dualshock4_touch_point {
    uint8_t contact;
    uint8_t x_lo;
    uint8_t x_hi:4, y_lo:4;
    uint8_t y_hi;
};

struct __attribute__((packed)) dualshock4_touch_report {
    uint8_t timestamp;
    struct dualshock4_touch_point points[2];
};

struct __attribute__((packed)) dualshock4_input_report_bt {
    uint8_t report_id; // 0x11
    uint8_t reserved[2];
    struct dualshock4_input_report_common common;
    uint8_t num_touch_reports;
    struct dualshock4_touch_report touch_reports[4];
    uint8_t reserved2[2];
    uint32_t crc32;
};

struct __attribute__((packed)) dualshock4_input_report_usb {
    uint8_t report_id; // 0x01
    struct dualshock4_input_report_common common;
    uint8_t num_touch_reports;
    struct dualshock4_touch_report touch_reports[3];
    uint8_t reserved[3];
};

struct __attribute__((packed)) dualsense_touch_point {
    uint8_t contact;
    uint8_t x_lo;
    uint8_t x_hi:4, y_lo:4;
    uint8_t y_hi;
};

struct __attribute__((packed)) dualsense_input_report {
    uint8_t x, y;
    uint8_t rx, ry;
    uint8_t z, rz;
    uint8_t seq_number;
    uint8_t buttons[4];
    uint8_t reserved[4];
    int16_t gyro[3];
    int16_t accel[3];
    uint32_t sensor_timestamp;
    uint8_t reserved2;
    struct dualsense_touch_point points[2];
    uint8_t reserved3[12];
    uint8_t status[3];
    uint8_t reserved4[8];
};

// Scan /dev/ for physical DualShock 4
std::string find_physical_ds4(bool& out_is_bluetooth) {
    std::string found_name = "";
    for (const auto& entry : fs::directory_iterator("/dev")) {
        std::string name = entry.path().filename().string();
        if (name.rfind("hidraw", 0) == 0) {
            std::string dev_path = "/dev/" + name;
            int fd = open(dev_path.c_str(), O_RDWR | O_NONBLOCK);
            if (fd >= 0) {
                struct hidraw_devinfo info;
                if (ioctl(fd, HIDIOCGRAWINFO, &info) >= 0) {
                    if (info.vendor == 0x054c && (info.product == 0x05c4 || info.product == 0x09cc)) {
                        if (info.bustype == 0x05) { // BUS_BLUETOOTH
                            close(fd);
                            out_is_bluetooth = true;
                            return name;
                        } else {
                            found_name = name;
                            out_is_bluetooth = false;
                        }
                    }
                }
                close(fd);
            }
        }
    }
    return found_name;
}

// Find event nodes for a hidraw device
std::vector<std::string> get_event_nodes(const std::string& hidraw_name) {
    std::vector<std::string> event_nodes;
    std::string path = "/sys/class/hidraw/" + hidraw_name + "/device/input";
    if (fs::exists(path)) {
        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.is_directory()) {
                for (const auto& subentry : fs::directory_iterator(entry.path())) {
                    std::string name = subentry.path().filename().string();
                    if (name.rfind("event", 0) == 0 || name.rfind("js", 0) == 0) {
                        event_nodes.push_back("/dev/input/" + name);
                    }
                }
            }
        }
    }
    return event_nodes;
}

// Send output report to physical controller
void send_physical_output_report(int fd, bool is_bluetooth, uint8_t motor_left, uint8_t motor_right, uint8_t r, uint8_t g, uint8_t b) {
    if (is_bluetooth) {
        uint8_t buf[78];
        memset(buf, 0, sizeof(buf));
        buf[0] = 0x11; // Report ID
        buf[1] = 0xC0; // hw_control: HID | CRC32
        buf[2] = 0x00; // audio_control
        buf[3] = 0x07; // valid_flag0: motor | led | led_blink
        buf[4] = 0x00; // valid_flag1
        buf[5] = 0x00; // reserved
        buf[6] = motor_right;
        buf[7] = motor_left;
        buf[8] = r;
        buf[9] = g;
        buf[10] = b;
        
        uint32_t crc = calculate_crc32(0xA2, buf, 74);
        buf[74] = crc & 0xFF;
        buf[75] = (crc >> 8) & 0xFF;
        buf[76] = (crc >> 16) & 0xFF;
        buf[77] = (crc >> 24) & 0xFF;
        
        if (write(fd, buf, 78) < 0) {
            std::cerr << "Failed to write Bluetooth output report to physical controller" << std::endl;
        }
    } else {
        uint8_t buf[32];
        memset(buf, 0, sizeof(buf));
        buf[0] = 0x05; // Report ID
        buf[1] = 0x07; // valid_flag0: motor | led | led_blink
        buf[2] = 0x00; // valid_flag1
        buf[3] = 0x00; // reserved
        buf[4] = motor_right;
        buf[5] = motor_left;
        buf[6] = r;
        buf[7] = g;
        buf[8] = b;
        
        if (write(fd, buf, 32) < 0) {
            std::cerr << "Failed to write USB output report to physical controller" << std::endl;
        }
    }
}

// UHID Helper to write events
int uhid_write(int fd, const struct uhid_event& ev) {
    ssize_t ret = write(fd, &ev, sizeof(ev));
    if (ret < 0) {
        std::cerr << "uhid write failed: " << strerror(errno) << std::endl;
        return -1;
    }
    return 0;
}

// Setup IPC Unix Domain Socket
int setup_ipc_socket() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "Failed to create IPC socket: " << strerror(errno) << std::endl;
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/run/ds4-translator.sock", sizeof(addr.sun_path) - 1);

    unlink(addr.sun_path);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind IPC socket: " << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }

    if (listen(fd, 5) < 0) {
        std::cerr << "Failed to listen on IPC socket: " << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }

    // Set permissions to allow non-root status check/configuration
    chmod(addr.sun_path, 0666);

    return fd;
}

ControllerType read_config(ControllerType default_type) {
    std::ifstream f("/etc/ds4-translator.conf");
    if (!f.is_open()) {
        return default_type;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("type=", 0) == 0) {
            std::string val = line.substr(5);
            while (!val.empty() && (val.back() == '\n' || val.back() == '\r' || val.back() == ' ')) {
                val.pop_back();
            }
            if (val == "dualsense") {
                return TYPE_DUALSENSE;
            } else if (val == "ds4") {
                return TYPE_DS4;
            } else if (val == "none") {
                return TYPE_NONE;
            }
        }
    }
    return default_type;
}

void write_config(ControllerType type) {
    std::ofstream f("/etc/ds4-translator.conf");
    if (f.is_open()) {
        f << "type=" << (type == TYPE_DS4 ? "ds4" : (type == TYPE_NONE ? "none" : "dualsense")) << "\n";
    } else {
        std::cerr << "Failed to write config file: /etc/ds4-translator.conf: " << strerror(errno) << std::endl;
    }
}

int main(int argc, char* argv[]) {
    ControllerType target_type = TYPE_DS4;
    bool type_explicitly_set = false;
    
    // Command line parsing
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--type" || arg == "-t") {
            if (i + 1 < argc) {
                std::string val = argv[++i];
                if (val == "dualsense") {
                    target_type = TYPE_DUALSENSE;
                    type_explicitly_set = true;
                } else if (val == "ds4") {
                    target_type = TYPE_DS4;
                    type_explicitly_set = true;
                } else if (val == "none") {
                    target_type = TYPE_NONE;
                    type_explicitly_set = true;
                } else {
                    std::cerr << "Unknown type: " << val << ". Supported: ds4, dualsense, none" << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "Missing value for type option." << std::endl;
                return 1;
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: ds4-translator [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -t, --type <ds4|dualsense>   Target virtual controller type (default: ds4)" << std::endl;
            std::cout << "  -h, --help                  Show this help message" << std::endl;
            return 0;
        }
    }

    if (!type_explicitly_set) {
        target_type = read_config(target_type);
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "Starting DS4 Translator daemon..." << std::endl;
    std::cout << "Initial Target Emulation: " << (target_type == TYPE_DS4 ? "DualShock 4" : (target_type == TYPE_NONE ? "None" : "DualSense")) << std::endl;

    int server_fd = setup_ipc_socket();
    if (server_fd < 0) {
        std::cerr << "Continuing without IPC socket support." << std::endl;
    } else {
        std::cout << "IPC Unix socket created at /run/ds4-translator.sock" << std::endl;
    }

    if (target_type == TYPE_NONE) {
        std::ofstream f("/run/ds4-translator.none");
        f.close();
        system("udevadm trigger");
    } else {
        unlink("/run/ds4-translator.none");
    }

    int phy_fd = -1;
    int uhid_fd = -1;
    std::string phy_name = "";
    bool is_bluetooth = false;
    std::vector<PhysicalNode> hidden_nodes;
    std::string phy_path = "";
    mode_t orig_mode = 0660;

    bool device_open = false;
    uint8_t cur_motor_left = 0, cur_motor_right = 0;
    uint8_t cur_r = 0, cur_g = 0, cur_b = 255;
    uint8_t sequence_number = 0;

    bool type_change_requested = false;
    ControllerType pending_type_change = target_type;

    while (running) {
        // If physical controller disconnected, scan & setup
        if (phy_fd < 0 && target_type != TYPE_NONE) {
            bool bt = false;
            std::string name = find_physical_ds4(bt);
            if (!name.empty()) {
                std::cout << "Found physical DualShock 4: /dev/" << name 
                          << " (Connection: " << (bt ? "Bluetooth" : "USB") << ")" << std::endl;
                
                phy_name = name;
                is_bluetooth = bt;
                phy_path = "/dev/" + phy_name;

                // Save permissions and mask it (chmod 000)
                struct stat phy_st;
                orig_mode = 0660;
                if (stat(phy_path.c_str(), &phy_st) == 0) {
                    orig_mode = phy_st.st_mode & 0777;
                }
                if (chmod(phy_path.c_str(), 000) < 0) {
                    std::cerr << "Warning: Failed to hide physical controller permissions: " << strerror(errno) << std::endl;
                }
                system(("setfacl -b " + phy_path + " 2>/dev/null").c_str());

                phy_fd = open(phy_path.c_str(), O_RDWR | O_NONBLOCK);
                if (phy_fd < 0) {
                    std::cerr << "Failed to open physical controller: " << strerror(errno) << std::endl;
                    chmod(phy_path.c_str(), orig_mode);
                    phy_fd = -1;
                    phy_name = "";
                } else {
                    // Grab and hide input events (event and js nodes)
                    std::vector<std::string> event_paths = get_event_nodes(phy_name);
                    for (const auto& ev_path : event_paths) {
                        PhysicalNode node;
                        node.path = ev_path;
                        node.fd = -1;
                        node.orig_mode = 0660;
                        node.is_grabbed = false;

                        struct stat node_st;
                        if (stat(ev_path.c_str(), &node_st) == 0) {
                            node.orig_mode = node_st.st_mode & 0777;
                        }

                        if (chmod(ev_path.c_str(), 000) < 0) {
                            std::cerr << "Warning: Failed to hide input node " << ev_path << ": " << strerror(errno) << std::endl;
                        }
                        system(("setfacl -b " + ev_path + " 2>/dev/null").c_str());

                        // We only grab event nodes, not js nodes (EVIOCGRAB is only for evdev)
                        if (ev_path.find("event") != std::string::npos) {
                            int ev_fd = open(ev_path.c_str(), O_RDONLY | O_NONBLOCK);
                            if (ev_fd >= 0) {
                                if (ioctl(ev_fd, EVIOCGRAB, 1) >= 0) {
                                    node.fd = ev_fd;
                                    node.is_grabbed = true;
                                    std::cout << "Successfully grabbed and hid event node: " << ev_path << std::endl;
                                } else {
                                    std::cerr << "Warning: Failed to grab event node " << ev_path << ": " << strerror(errno) << std::endl;
                                    close(ev_fd);
                                }
                            }
                        } else {
                            std::cout << "Successfully hid joystick node: " << ev_path << std::endl;
                        }
                        hidden_nodes.push_back(node);
                    }

                    // Open UHID
                    uhid_fd = open("/dev/uhid", O_RDWR | O_CLOEXEC | O_NONBLOCK);
                    if (uhid_fd < 0) {
                        std::cerr << "Failed to open /dev/uhid: " << strerror(errno) << std::endl;
                        for (auto& node : hidden_nodes) {
                            if (node.is_grabbed && node.fd >= 0) {
                                ioctl(node.fd, EVIOCGRAB, 0);
                                close(node.fd);
                            }
                            chmod(node.path.c_str(), node.orig_mode);
                        }
                        hidden_nodes.clear();
                        close(phy_fd);
                        phy_fd = -1;
                        chmod(phy_path.c_str(), orig_mode);
                        phy_name = "";
                    } else {
                        // Create virtual controller
                        struct uhid_event ev;
                        memset(&ev, 0, sizeof(ev));
                        ev.type = UHID_CREATE2;
                        
                        if (target_type == TYPE_DS4) {
                            strncpy((char*)ev.u.create2.name, "Sony Computer Entertainment Wireless Controller", sizeof(ev.u.create2.name));
                            strncpy((char*)ev.u.create2.uniq, "74:e7:d6:3a:47:e8", sizeof(ev.u.create2.uniq));
                            ev.u.create2.rd_size = sizeof(ds4_usb_rdesc);
                            memcpy(ev.u.create2.rd_data, ds4_usb_rdesc, sizeof(ds4_usb_rdesc));
                            ev.u.create2.bus = BUS_USB;
                            ev.u.create2.vendor = 0x054c;
                            ev.u.create2.product = 0x05c4;
                            ev.u.create2.version = 0x8111;
                            ev.u.create2.country = 0;
                        } else {
                            strncpy((char*)ev.u.create2.name, "Sony Interactive Entertainment DualSense Wireless Controller", sizeof(ev.u.create2.name));
                            strncpy((char*)ev.u.create2.uniq, "74:e7:d6:3a:47:e8", sizeof(ev.u.create2.uniq));
                            ev.u.create2.rd_size = sizeof(dualsense_usb_rdesc);
                            memcpy(ev.u.create2.rd_data, dualsense_usb_rdesc, sizeof(dualsense_usb_rdesc));
                            ev.u.create2.bus = BUS_USB;
                            ev.u.create2.vendor = 0x054c;
                            ev.u.create2.product = 0x0ce6;
                            ev.u.create2.version = 0x8111;
                            ev.u.create2.country = 0;
                        }

                        if (uhid_write(uhid_fd, ev) < 0) {
                            close(uhid_fd);
                            uhid_fd = -1;
                            for (auto& node : hidden_nodes) {
                                if (node.is_grabbed && node.fd >= 0) {
                                    ioctl(node.fd, EVIOCGRAB, 0);
                                    close(node.fd);
                                }
                                chmod(node.path.c_str(), node.orig_mode);
                            }
                            hidden_nodes.clear();
                            close(phy_fd);
                            phy_fd = -1;
                            chmod(phy_path.c_str(), orig_mode);
                            phy_name = "";
                        } else {
                            if (target_type != TYPE_NONE) {
                                usleep(100000); // 100ms settling delay for udev properties
                                send_physical_output_report(phy_fd, is_bluetooth, 0, 0, cur_r, cur_g, cur_b);
                            }
                            device_open = false;
                        }
                    }
                }
            }
        }

        // Build poll FD set
        std::vector<struct pollfd> pfds;
        int phy_poll_idx = -1;
        int uhid_poll_idx = -1;
        int server_poll_idx = -1;

        if (phy_fd >= 0) {
            struct pollfd p;
            p.fd = phy_fd;
            p.events = POLLIN;
            pfds.push_back(p);
            phy_poll_idx = pfds.size() - 1;
        }
        if (uhid_fd >= 0) {
            struct pollfd p;
            p.fd = uhid_fd;
            p.events = POLLIN;
            pfds.push_back(p);
            uhid_poll_idx = pfds.size() - 1;
        }
        if (server_fd >= 0) {
            struct pollfd p;
            p.fd = server_fd;
            p.events = POLLIN;
            pfds.push_back(p);
            server_poll_idx = pfds.size() - 1;
        }

        int poll_ret = poll(pfds.data(), pfds.size(), 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "Poll failed: " << strerror(errno) << std::endl;
            break;
        }

        // Handle Unix Domain Socket configuration clients
        if (server_poll_idx >= 0 && (pfds[server_poll_idx].revents & POLLIN)) {
            int client_fd = accept(server_fd, nullptr, nullptr);
            if (client_fd >= 0) {
                char rx_buf[256];
                memset(rx_buf, 0, sizeof(rx_buf));
                ssize_t bytes_read = read(client_fd, rx_buf, sizeof(rx_buf) - 1);
                if (bytes_read > 0) {
                    std::string cmd(rx_buf);
                    while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) {
                        cmd.pop_back();
                    }

                    std::string response = "Unknown command";
                    if (cmd == "status") {
                        response = "Physical Controller: " + (phy_name.empty() ? "None" : "/dev/" + phy_name) + "\n";
                        response += "Connection Type: " + std::string(phy_fd >= 0 ? (is_bluetooth ? "Bluetooth" : "USB") : "N/A") + "\n";
                        response += "Virtual Emulation: " + std::string(target_type == TYPE_DS4 ? "DualShock 4" : (target_type == TYPE_NONE ? "None" : "DualSense")) + "\n";
                        response += "Device Open by Host: " + std::string(device_open ? "Yes" : "No") + "\n";
                    } else if (cmd == "set-type ds4") {
                        if (target_type != TYPE_DS4) {
                            pending_type_change = TYPE_DS4;
                            type_change_requested = true;
                            write_config(TYPE_DS4);
                            response = "OK: Changing emulation type to DualShock 4...";
                        } else {
                            response = "Already set to DualShock 4";
                        }
                    } else if (cmd == "set-type dualsense") {
                        if (target_type != TYPE_DUALSENSE) {
                            pending_type_change = TYPE_DUALSENSE;
                            type_change_requested = true;
                            write_config(TYPE_DUALSENSE);
                            response = "OK: Changing emulation type to DualSense...";
                        } else {
                            response = "Already set to DualSense";
                        }
                    } else if (cmd == "set-type none") {
                        if (target_type != TYPE_NONE) {
                            pending_type_change = TYPE_NONE;
                            type_change_requested = true;
                            write_config(TYPE_NONE);
                            response = "OK: Changing emulation type to None (translation disabled)...";
                        } else {
                            response = "Already set to None";
                        }
                    }
                    write(client_fd, response.c_str(), response.size());
                }
                close(client_fd);
            }
        }

        // Recreate UHID device on dynamic emulation type reload
        if (type_change_requested) {
            target_type = pending_type_change;
            type_change_requested = false;
            
            if (uhid_fd >= 0) {
                struct uhid_event destroy_ev;
                memset(&destroy_ev, 0, sizeof(destroy_ev));
                destroy_ev.type = UHID_DESTROY;
                uhid_write(uhid_fd, destroy_ev);
                close(uhid_fd);
                uhid_fd = -1;
            }

            // Release physical controller grab and permissions
            if (phy_fd >= 0) {
                std::cout << "Releasing physical controller grab..." << std::endl;
                for (auto& node : hidden_nodes) {
                    if (node.is_grabbed && node.fd >= 0) {
                        ioctl(node.fd, EVIOCGRAB, 0);
                        close(node.fd);
                    }
                    chmod(node.path.c_str(), node.orig_mode);
                }
                hidden_nodes.clear();
                close(phy_fd);
                phy_fd = -1;
                chmod(phy_path.c_str(), orig_mode);
                phy_name = "";
            }

            if (target_type == TYPE_NONE) {
                std::cout << "Emulation type set to None. Disabling translation." << std::endl;
                std::ofstream f("/run/ds4-translator.none");
                f.close();
                system("udevadm trigger");
            } else {
                unlink("/run/ds4-translator.none");
                system("udevadm trigger");

                std::cout << "Recreating virtual controller as " << (target_type == TYPE_DS4 ? "DualShock 4" : "DualSense") << std::endl;
                uhid_fd = open("/dev/uhid", O_RDWR | O_CLOEXEC | O_NONBLOCK);
                if (uhid_fd >= 0) {
                    struct uhid_event ev;
                    memset(&ev, 0, sizeof(ev));
                    ev.type = UHID_CREATE2;
                    
                    if (target_type == TYPE_DS4) {
                        strncpy((char*)ev.u.create2.name, "Sony Computer Entertainment Wireless Controller", sizeof(ev.u.create2.name));
                        strncpy((char*)ev.u.create2.uniq, "74:e7:d6:3a:47:e8", sizeof(ev.u.create2.uniq));
                        ev.u.create2.rd_size = sizeof(ds4_usb_rdesc);
                        memcpy(ev.u.create2.rd_data, ds4_usb_rdesc, sizeof(ds4_usb_rdesc));
                        ev.u.create2.bus = BUS_USB;
                        ev.u.create2.vendor = 0x054c;
                        ev.u.create2.product = 0x05c4;
                        ev.u.create2.version = 0x8111;
                        ev.u.create2.country = 0;
                    } else {
                        strncpy((char*)ev.u.create2.name, "Sony Interactive Entertainment DualSense Wireless Controller", sizeof(ev.u.create2.name));
                        strncpy((char*)ev.u.create2.uniq, "74:e7:d6:3a:47:e8", sizeof(ev.u.create2.uniq));
                        ev.u.create2.rd_size = sizeof(dualsense_usb_rdesc);
                        memcpy(ev.u.create2.rd_data, dualsense_usb_rdesc, sizeof(dualsense_usb_rdesc));
                        ev.u.create2.bus = BUS_USB;
                        ev.u.create2.vendor = 0x054c;
                        ev.u.create2.product = 0x0ce6;
                        ev.u.create2.version = 0x8111;
                        ev.u.create2.country = 0;
                    }

                    if (uhid_write(uhid_fd, ev) >= 0) {
                        std::cout << "Virtual USB Controller created (reloaded)." << std::endl;
                        device_open = false;
                        usleep(100000); // 100ms settling delay for udev properties
                        send_physical_output_report(phy_fd, is_bluetooth, 0, 0, cur_r, cur_g, cur_b);
                    } else {
                        close(uhid_fd);
                        uhid_fd = -1;
                    }
                }
            }
        }

        // Handle physical controller connection dropped
        if (phy_poll_idx >= 0 && (pfds[phy_poll_idx].revents & (POLLERR | POLLHUP))) {
            std::cout << "Physical controller connection dropped." << std::endl;
            for (auto& node : hidden_nodes) {
                if (node.is_grabbed && node.fd >= 0) {
                    ioctl(node.fd, EVIOCGRAB, 0);
                    close(node.fd);
                }
                chmod(node.path.c_str(), node.orig_mode);
            }
            hidden_nodes.clear();
            close(phy_fd);
            phy_fd = -1;
            chmod(phy_path.c_str(), orig_mode);
            phy_name = "";

            if (uhid_fd >= 0) {
                struct uhid_event destroy_ev;
                memset(&destroy_ev, 0, sizeof(destroy_ev));
                destroy_ev.type = UHID_DESTROY;
                uhid_write(uhid_fd, destroy_ev);
                close(uhid_fd);
                uhid_fd = -1;
            }
            continue;
        }

        // Forward physical controller inputs to virtual controller
        if (phy_poll_idx >= 0 && (pfds[phy_poll_idx].revents & POLLIN)) {
            uint8_t in_buf[128];
            ssize_t bytes_read = read(phy_fd, in_buf, sizeof(in_buf));
            if (bytes_read <= 0) {
                if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    // ignore
                } else {
                    std::cerr << "Error reading physical controller: " << strerror(errno) << std::endl;
                    // Trigger disconnect manually
                    pfds[phy_poll_idx].revents |= POLLHUP;
                }
            } else if (device_open && uhid_fd >= 0) {
                struct dualshock4_input_report_common common;
                bool got_report = false;
                uint8_t num_touch = 0;
                struct dualshock4_touch_report touch_reps[4];
                memset(&common, 0, sizeof(common));
                memset(touch_reps, 0, sizeof(touch_reps));

                if (in_buf[0] == 0x01 && bytes_read >= 64) {
                    struct dualshock4_input_report_usb* usb_in = (struct dualshock4_input_report_usb*)in_buf;
                    common = usb_in->common;
                    num_touch = usb_in->num_touch_reports;
                    for (int i = 0; i < 3 && i < num_touch; ++i) {
                        touch_reps[i] = usb_in->touch_reports[i];
                    }
                    got_report = true;
                } else if (in_buf[0] == 0x11 && bytes_read >= 78) {
                    struct dualshock4_input_report_bt* bt_in = (struct dualshock4_input_report_bt*)in_buf;
                    common = bt_in->common;
                    num_touch = bt_in->num_touch_reports;
                    for (int i = 0; i < 4 && i < num_touch; ++i) {
                        touch_reps[i] = bt_in->touch_reports[i];
                    }
                    got_report = true;
                }

                if (got_report) {
                    struct uhid_event out_ev;
                    memset(&out_ev, 0, sizeof(out_ev));
                    out_ev.type = UHID_INPUT2;

                    if (target_type == TYPE_DS4) {
                        out_ev.u.input2.size = sizeof(struct dualshock4_input_report_usb);
                        struct dualshock4_input_report_usb* out_ds = (struct dualshock4_input_report_usb*)out_ev.u.input2.data;
                        out_ds->report_id = 0x01;
                        out_ds->common = common;
                        out_ds->num_touch_reports = (num_touch > 3) ? 3 : num_touch;
                        for (int i = 0; i < out_ds->num_touch_reports; ++i) {
                            out_ds->touch_reports[i] = touch_reps[i];
                        }
                    } else { // DualSense
                        out_ev.u.input2.size = 64;
                        out_ev.u.input2.data[0] = 0x01;
                        struct dualsense_input_report* out_ds5 = (struct dualsense_input_report*)&out_ev.u.input2.data[1];
                        
                        out_ds5->x = common.x;
                        out_ds5->y = common.y;
                        out_ds5->rx = common.rx;
                        out_ds5->ry = common.ry;
                        
                        out_ds5->z = common.z;
                        out_ds5->rz = common.rz;
                        
                        out_ds5->buttons[0] = common.buttons[0];
                        out_ds5->buttons[1] = common.buttons[1];
                        out_ds5->buttons[2] = common.buttons[2] & 0x03;
                        out_ds5->buttons[3] = 0;
                        
                        out_ds5->seq_number = sequence_number++;
                        memcpy(out_ds5->gyro, common.gyro, sizeof(out_ds5->gyro));
                        memcpy(out_ds5->accel, common.accel, sizeof(out_ds5->accel));
                        out_ds5->sensor_timestamp = (uint32_t)common.sensor_timestamp;
                        
                        if (num_touch > 0) {
                            int latest = num_touch - 1;
                            if (latest > 3) latest = 3;
                            out_ds5->points[0].contact = touch_reps[latest].points[0].contact;
                            out_ds5->points[0].x_lo = touch_reps[latest].points[0].x_lo;
                            out_ds5->points[0].x_hi = touch_reps[latest].points[0].x_hi;
                            out_ds5->points[0].y_lo = touch_reps[latest].points[0].y_lo;
                            out_ds5->points[0].y_hi = touch_reps[latest].points[0].y_hi;

                            out_ds5->points[1].contact = touch_reps[latest].points[1].contact;
                            out_ds5->points[1].x_lo = touch_reps[latest].points[1].x_lo;
                            out_ds5->points[1].x_hi = touch_reps[latest].points[1].x_hi;
                            out_ds5->points[1].y_lo = touch_reps[latest].points[1].y_lo;
                            out_ds5->points[1].y_hi = touch_reps[latest].points[1].y_hi;
                        } else {
                            out_ds5->points[0].contact = 0x80;
                            out_ds5->points[1].contact = 0x80;
                        }
                        
                        out_ds5->status[0] = 0x2B; // Fully charged, complete
                        out_ds5->status[1] = 0x00;
                        out_ds5->status[2] = 0x00;
                    }

                    uhid_write(uhid_fd, out_ev);
                }
            }
        }

        // Handle virtual controller state/queries from Kernel
        if (uhid_poll_idx >= 0 && (pfds[uhid_poll_idx].revents & (POLLERR | POLLHUP))) {
            std::cerr << "UHID device dropped." << std::endl;
            // Trigger recreate of virtual device
            close(uhid_fd);
            uhid_fd = -1;
            continue;
        }

        if (uhid_poll_idx >= 0 && (pfds[uhid_poll_idx].revents & POLLIN)) {
            struct uhid_event kernel_ev;
            ssize_t bytes_read = read(uhid_fd, &kernel_ev, sizeof(kernel_ev));
            if (bytes_read < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    std::cerr << "Error reading from uhid: " << strerror(errno) << std::endl;
                    close(uhid_fd);
                    uhid_fd = -1;
                }
            } else if (bytes_read > 0) {
                switch (kernel_ev.type) {
                    case UHID_START:
                        std::cout << "UHID Device Started by kernel" << std::endl;
                        break;
                    case UHID_STOP:
                        std::cout << "UHID Device Stopped by kernel" << std::endl;
                        break;
                    case UHID_OPEN:
                        std::cout << "UHID Device Opened by host" << std::endl;
                        device_open = true;
                        break;
                    case UHID_CLOSE:
                        std::cout << "UHID Device Closed by host" << std::endl;
                        device_open = false;
                        break;
                    case UHID_GET_REPORT: {
                        struct uhid_event reply_ev;
                        memset(&reply_ev, 0, sizeof(reply_ev));
                        reply_ev.type = UHID_GET_REPORT_REPLY;
                        reply_ev.u.get_report_reply.id = kernel_ev.u.get_report.id;
                        reply_ev.u.get_report_reply.err = 0;
                        
                        uint8_t rnum = kernel_ev.u.get_report.rnum;
                        uint8_t rtype = kernel_ev.u.get_report.rtype;
                        
                        if (rtype == UHID_FEATURE_REPORT) {
                            if (target_type == TYPE_DS4) {
                                if (rnum == 0x02) {
                                    reply_ev.u.get_report_reply.size = 37;
                                    reply_ev.u.get_report_reply.data[0] = 0x02;
                                    // Mock calibration
                                    reply_ev.u.get_report_reply.data[7] = 0x00; reply_ev.u.get_report_reply.data[8] = 0x04;
                                    reply_ev.u.get_report_reply.data[9] = 0x00; reply_ev.u.get_report_reply.data[10] = 0xFC;
                                    reply_ev.u.get_report_reply.data[11] = 0x00; reply_ev.u.get_report_reply.data[12] = 0x04;
                                    reply_ev.u.get_report_reply.data[13] = 0x00; reply_ev.u.get_report_reply.data[14] = 0xFC;
                                    reply_ev.u.get_report_reply.data[15] = 0x00; reply_ev.u.get_report_reply.data[16] = 0x04;
                                    reply_ev.u.get_report_reply.data[17] = 0x00; reply_ev.u.get_report_reply.data[18] = 0xFC;
                                    reply_ev.u.get_report_reply.data[19] = 0x00; reply_ev.u.get_report_reply.data[20] = 0x04;
                                    reply_ev.u.get_report_reply.data[21] = 0x00; reply_ev.u.get_report_reply.data[22] = 0x04;
                                    reply_ev.u.get_report_reply.data[23] = 0x00; reply_ev.u.get_report_reply.data[24] = 0x20;
                                    reply_ev.u.get_report_reply.data[25] = 0x00; reply_ev.u.get_report_reply.data[26] = 0xE0;
                                    reply_ev.u.get_report_reply.data[27] = 0x00; reply_ev.u.get_report_reply.data[28] = 0x20;
                                    reply_ev.u.get_report_reply.data[29] = 0x00; reply_ev.u.get_report_reply.data[30] = 0xE0;
                                    reply_ev.u.get_report_reply.data[31] = 0x00; reply_ev.u.get_report_reply.data[32] = 0x20;
                                    reply_ev.u.get_report_reply.data[33] = 0x00; reply_ev.u.get_report_reply.data[34] = 0xE0;
                                } else if (rnum == 0xa3) {
                                    reply_ev.u.get_report_reply.size = 49;
                                    reply_ev.u.get_report_reply.data[0] = 0xa3;
                                    // Real DS4 hw_version = 0x5438, fw_version = 0x2033
                                    reply_ev.u.get_report_reply.data[35] = 0x38;
                                    reply_ev.u.get_report_reply.data[36] = 0x54;
                                    reply_ev.u.get_report_reply.data[41] = 0x33;
                                    reply_ev.u.get_report_reply.data[42] = 0x20;
                                } else if (rnum == 0x12) {
                                    reply_ev.u.get_report_reply.size = 16;
                                    reply_ev.u.get_report_reply.data[0] = 0x12;
                                    reply_ev.u.get_report_reply.data[1] = 0xe8;
                                    reply_ev.u.get_report_reply.data[2] = 0x47;
                                    reply_ev.u.get_report_reply.data[3] = 0x3a;
                                    reply_ev.u.get_report_reply.data[4] = 0xd6;
                                    reply_ev.u.get_report_reply.data[5] = 0xe7;
                                    reply_ev.u.get_report_reply.data[6] = 0x74;
                                }
                            } else { // DualSense
                                if (rnum == 0x05) {
                                    reply_ev.u.get_report_reply.size = 41;
                                    uint8_t cal_data[41] = {
                                        0x05,
                                        0xff, 0xfc, 0xff, 0xfe, 0xff, 0x83, 0x22, 0x78,
                                        0xdd, 0x92, 0x22, 0x5f, 0xdd, 0x95, 0x22, 0x6d,
                                        0xdd, 0x1c, 0x02, 0x1c, 0x02, 0xf2, 0x1f, 0xed,
                                        0xdf, 0xe3, 0x20, 0xda, 0xe0, 0xee, 0x1f, 0xdf,
                                        0xdf, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                                    };
                                    memcpy(reply_ev.u.get_report_reply.data, cal_data, 41);
                                } else if (rnum == 0x20) {
                                    reply_ev.u.get_report_reply.size = 64;
                                    uint8_t fw_data[64] = {
                                        0x20,
                                        0x4a, 0x75, 0x6e, 0x20, 0x31, 0x39, 0x20, 0x32, 0x30, 0x32, 0x33,
                                        0x31, 0x34, 0x3a, 0x34, 0x37, 0x3a, 0x33, 0x34,
                                        0x03, 0x00, 0x44, 0x00, 0x08, 0x02, 0x00, 0x01,
                                        0x36, 0x00, 0x00, 0x01, 0xc1, 0xc8, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x54, 0x01, 0x00, 0x00, 0x14, 0x00,
                                        0x00, 0x00, 0x0b, 0x00, 0x01, 0x00, 0x06, 0x00,
                                        0x00, 0x00, 0x00, 0x00
                                    };
                                    memcpy(reply_ev.u.get_report_reply.data, fw_data, 64);
                                } else if (rnum == 0x09) {
                                    reply_ev.u.get_report_reply.size = 20;
                                    uint8_t pairing_data[20] = {
                                        0x09,
                                        0xe8, 0x47, 0x3a, 0xd6, 0xe7, 0x74,
                                        0x08, 0x25, 0x00, 0x1e, 0x00, 0xee, 0x74, 0xd0, 0xbc,
                                        0x00, 0x00, 0x00, 0x00
                                    };
                                    memcpy(reply_ev.u.get_report_reply.data, pairing_data, 20);
                                }
                            }
                        }
                        uhid_write(uhid_fd, reply_ev);
                        break;
                    }
                    case UHID_SET_REPORT: {
                        struct uhid_event reply_ev;
                        memset(&reply_ev, 0, sizeof(reply_ev));
                        reply_ev.type = UHID_SET_REPORT_REPLY;
                        reply_ev.u.set_report_reply.id = kernel_ev.u.set_report.id;
                        reply_ev.u.set_report_reply.err = 0;
                        uhid_write(uhid_fd, reply_ev);
                        break;
                    }
                    case UHID_OUTPUT: {
                        uint16_t size = kernel_ev.u.output.size;
                        const uint8_t* data = kernel_ev.u.output.data;
                        
                        uint8_t motor_left = cur_motor_left;
                        uint8_t motor_right = cur_motor_right;
                        uint8_t r = cur_r, g = cur_g, b = cur_b;
                        bool update = false;
                        
                        if (target_type == TYPE_DS4) {
                            if (data[0] == 0x05 && size >= 9) {
                                uint8_t flags = data[1];
                                if (flags & 0x01) {
                                    motor_right = data[4];
                                    motor_left = data[5];
                                    update = true;
                                }
                                if (flags & 0x02) {
                                    r = data[6];
                                    g = data[7];
                                    b = data[8];
                                    update = true;
                                }
                            }
                        } else { // DualSense
                            if (data[0] == 0x02 && size >= 48) {
                                uint8_t vf0 = data[1];
                                uint8_t vf1 = data[2];
                                if (vf0 & 0x01) {
                                    motor_right = data[3];
                                    motor_left = data[4];
                                    // If haptics/sound-select (vf0 & 0x02) is active,
                                    // don't run both motors at the same time to prevent
                                    // the big motor from drowning out the light motor.
                                    if ((vf0 & 0x02) && motor_right > 0 && motor_left > 0) {
                                        motor_left = 0;
                                    }
                                    update = true;
                                }
                                if (vf1 & 0x04) {
                                    r = data[45];
                                    g = data[46];
                                    b = data[47];
                                    update = true;
                                }
                            }
                        }

                        if (update) {
                            if (motor_left != cur_motor_left || motor_right != cur_motor_right || r != cur_r || g != cur_g || b != cur_b) {
                                cur_motor_left = motor_left;
                                cur_motor_right = motor_right;
                                cur_r = r;
                                cur_g = g;
                                cur_b = b;
                                if (phy_fd >= 0) {
                                    send_physical_output_report(phy_fd, is_bluetooth, cur_motor_left, cur_motor_right, cur_r, cur_g, cur_b);
                                }
                            }
                        }
                        break;
                    }
                }
            }
        }
    }

    // Clean up Unix socket
    if (server_fd >= 0) {
        close(server_fd);
        unlink("/run/ds4-translator.sock");
    }

    // Clean up physical controller if active
    if (phy_fd >= 0) {
        std::cout << "Releasing physical controller grab..." << std::endl;
        for (auto& node : hidden_nodes) {
            if (node.is_grabbed && node.fd >= 0) {
                ioctl(node.fd, EVIOCGRAB, 0);
                close(node.fd);
            }
            chmod(node.path.c_str(), node.orig_mode);
        }
        close(phy_fd);
        chmod(phy_path.c_str(), orig_mode);
    }

    // Destroy virtual controller
    if (uhid_fd >= 0) {
        std::cout << "Destroying virtual device..." << std::endl;
        struct uhid_event destroy_ev;
        memset(&destroy_ev, 0, sizeof(destroy_ev));
        destroy_ev.type = UHID_DESTROY;
        uhid_write(uhid_fd, destroy_ev);
        close(uhid_fd);
    }

    std::cout << "DS4 Translator daemon stopped." << std::endl;
    return 0;
}
