#include "hid-bpf-transport.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// DS4's largest Bluetooth input report is 78 bytes -- matches
// usb-hid-transport.cpp's kMaxReportSize rationale.
constexpr size_t kMaxReportSize = 128;

// Baked in at build time (see Makefile) as the `make install` destination
// for the compiled BPF object.
#ifndef HID_BPF_OBJ_PATH
#define HID_BPF_OBJ_PATH "/usr/local/lib/ds4-translator/hid-bpf-transport.bpf.o"
#endif

struct TransportState {
    int hidraw_fd = -1;
    int ringbuf_map_fd = -1;
    struct ring_buffer* rb = nullptr;
    int sv[2] = {-1, -1};
    std::thread pump_thread;
    std::atomic<bool> stop_flag{false};
    std::string sysfs_path; // /sys/bus/hid/devices/<hid_id>, for teardown
};

std::mutex g_registry_mutex;
std::unordered_map<int, TransportState*> g_registry;

// Prefers the installed path but falls back to the build tree so `make`
// (without `make install`) still works for local testing.
std::string bpf_object_path() {
    std::error_code ec;
    if (fs::exists(HID_BPF_OBJ_PATH, ec)) return HID_BPF_OBJ_PATH;
    if (fs::exists("build/hid-bpf-transport.bpf.o", ec)) return "build/hid-bpf-transport.bpf.o";
    return HID_BPF_OBJ_PATH; // let the caller's own exists-check report the real error
}

// Runs `udev-hid-bpf <args...>` via fork/execvp -- no shell, so hid_id-
// or path-derived arguments never pass through shell interpolation.
// Returns true only on a real zero exit status (a missing binary makes
// the child _exit(127), which this correctly reports as failure).
bool run_udev_hid_bpf(const std::vector<std::string>& args) {
    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "hid-bpf: fork failed: " << strerror(errno) << std::endl;
        return false;
    }
    if (pid == 0) {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("udev-hid-bpf"));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp("udev-hid-bpf", argv.data());
        _exit(127); // execvp only returns on failure (e.g. not installed)
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// udev-hid-bpf pins every map for a loaded program under
// /sys/fs/bpf/hid/<hid_id-with-":"/"." replaced by "_">/<program-name-
// derived subdirectory>/ -- the subdirectory name is an internal detail
// of the loader we don't want to hardcode (confirmed via testing that it
// isn't simply the object's basename), so find our map by its known name
// instead of predicting the full path.
std::string find_pinned_ringbuf(const std::string& hid_id) {
    std::string prefix;
    for (char c : hid_id) prefix += (c == ':' || c == '.') ? '_' : c;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/fs/bpf/hid/" + prefix, ec)) {
        fs::path candidate = entry.path() / "ds4_reports";
        if (fs::exists(candidate, ec)) return candidate.string();
    }
    return "";
}

// Finds /dev/hidrawN for a hid_device's sysfs path -- the driver is
// never unbound under this transport, so this always exists as long as
// the controller is connected.
std::string find_hidraw_node(const std::string& sysfs_path) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(sysfs_path + "/hidraw", ec)) {
        return "/dev/" + entry.path().filename().string();
    }
    return "";
}

int on_ringbuf_report(void* ctx, void* data, size_t data_sz) {
    auto* t = static_cast<TransportState*>(ctx);
    ssize_t n = write(t->sv[0], data, data_sz);
    (void)n; // peer (sv[1], read by the daemon's main poll loop) closing
             // is caught by stop_flag on the pump thread's next check.
    return 0;
}

void pump_thread_fn(TransportState* t) {
    uint8_t out_buf[kMaxReportSize];
    int liveness_check_counter = 0;

    while (!t->stop_flag.load(std::memory_order_relaxed)) {
        // Outbound: forward at most one pending LED/rumble write per
        // iteration, straight to the still-live hidraw node -- mirrors
        // usb-hid-transport.cpp's pump_thread_fn exactly, since
        // hid_device_event never touches this direction.
        struct pollfd pfd;
        pfd.fd = t->sv[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(t->sv[0], out_buf, sizeof(out_buf));
            if (n > 0) {
                if (write(t->hidraw_fd, out_buf, (size_t)n) < 0) {
                    std::cerr << "hid-bpf: failed to write output report to "
                              << "physical controller: " << strerror(errno) << std::endl;
                }
            }
        }

        // Inbound: blocks up to 8ms (matches usb-hid-transport.cpp's own
        // steady-state granularity) so stop_flag/outbound writes are
        // still checked promptly; on_ringbuf_report() fires synchronously
        // from within this call for anything already queued.
        int err = ring_buffer__poll(t->rb, 8 /* ms */);
        if (err < 0 && err != -EINTR) {
            std::cerr << "hid-bpf: ring_buffer__poll: " << strerror(-err) << std::endl;
            break;
        }

        // Unlike libusb (which surfaces LIBUSB_ERROR_NO_DEVICE the moment
        // the physical device disappears), ring_buffer__poll() has no
        // analogous "the underlying hid_device is gone" error -- if
        // bluetoothd tears down and recreates the uhid session (a real
        // BT reconnect/re-pair cycle, or the controller simply dropping
        // and coming back), the kernel destroys this hid_device (and
        // with it, this struct_ops attachment and its ring buffer), and
        // this call just goes quietly silent forever: zero events, zero
        // errors. Confirmed live (2026-08-10) that this leaves phy_fd
        // looking permanently healthy days after the hid_id it was
        // opened against no longer exists in sysfs at all -- a fresh,
        // completely unmanaged hid_device shows up in its place with no
        // hiding applied to it whatsoever. Check roughly once a second
        // (~125 iterations at the 8ms poll granularity above) rather
        // than every iteration, since this is just a liveness check, not
        // the hot path.
        if (++liveness_check_counter >= 125) {
            liveness_check_counter = 0;
            std::error_code ec;
            if (!fs::exists(t->sysfs_path, ec)) {
                std::cerr << "hid-bpf: " << t->sysfs_path << " no longer exists "
                             "(physical controller disconnected or bluetoothd recreated the session)" << std::endl;
                break;
            }
        }
    }

    shutdown(t->sv[0], SHUT_RDWR);
}

} // namespace

int hid_bpf_transport_open(const std::string& hid_id, std::string& out_hidraw_name) {
    std::string sysfs_path = "/sys/bus/hid/devices/" + hid_id;
    std::string obj_path = bpf_object_path();
    std::error_code ec;
    if (!fs::exists(obj_path, ec)) {
        std::cerr << "hid-bpf: " << obj_path << " not found (build/install issue)" << std::endl;
        return -1;
    }

    if (!run_udev_hid_bpf({"add", sysfs_path, obj_path})) {
        std::cerr << "hid-bpf: udev-hid-bpf add failed for " << hid_id
                  << " (is the udev-hid-bpf package installed?)" << std::endl;
        return -1;
    }

    std::string pin_path = find_pinned_ringbuf(hid_id);
    if (pin_path.empty()) {
        std::cerr << "hid-bpf: couldn't find pinned ring buffer for " << hid_id << std::endl;
        run_udev_hid_bpf({"remove", sysfs_path});
        return -1;
    }

    int map_fd = bpf_obj_get(pin_path.c_str());
    if (map_fd < 0) {
        std::cerr << "hid-bpf: bpf_obj_get(" << pin_path << ") failed: " << strerror(errno) << std::endl;
        run_udev_hid_bpf({"remove", sysfs_path});
        return -1;
    }

    std::string hidraw_dev = find_hidraw_node(sysfs_path);
    if (hidraw_dev.empty()) {
        std::cerr << "hid-bpf: no hidraw node under " << sysfs_path << std::endl;
        close(map_fd);
        run_udev_hid_bpf({"remove", sysfs_path});
        return -1;
    }

    int hidraw_fd = open(hidraw_dev.c_str(), O_RDWR);
    if (hidraw_fd < 0) {
        std::cerr << "hid-bpf: open(" << hidraw_dev << "): " << strerror(errno) << std::endl;
        close(map_fd);
        run_udev_hid_bpf({"remove", sysfs_path});
        return -1;
    }
    out_hidraw_name = fs::path(hidraw_dev).filename().string();

    int sv[2];
    // SOCK_SEQPACKET, not SOCK_DGRAM -- see usb-hid-transport.h's doc
    // comment (this mirrors that transport's exact design): SOCK_DGRAM
    // here never delivers POLLHUP/EOF to the peer when this end closes
    // (confirmed live, 2026-08-10), leaving phy_fd looking permanently
    // healthy after the controller is actually gone -- reproduced with
    // both a real Bluetooth disconnect and a standalone socketpair test.
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
        std::cerr << "hid-bpf: socketpair failed: " << strerror(errno) << std::endl;
        close(hidraw_fd);
        close(map_fd);
        run_udev_hid_bpf({"remove", sysfs_path});
        return -1;
    }
    int flags = fcntl(sv[1], F_GETFL, 0);
    fcntl(sv[1], F_SETFL, flags | O_NONBLOCK);

    auto* t = new TransportState();
    t->hidraw_fd = hidraw_fd;
    t->ringbuf_map_fd = map_fd;
    t->sv[0] = sv[0];
    t->sv[1] = sv[1];
    t->sysfs_path = sysfs_path;

    t->rb = ring_buffer__new(map_fd, on_ringbuf_report, t, nullptr);
    if (!t->rb) {
        std::cerr << "hid-bpf: ring_buffer__new failed" << std::endl;
        close(sv[0]);
        close(sv[1]);
        close(hidraw_fd);
        close(map_fd);
        run_udev_hid_bpf({"remove", sysfs_path});
        delete t;
        return -1;
    }

    t->pump_thread = std::thread(pump_thread_fn, t);

    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        g_registry[sv[1]] = t;
    }

    return sv[1];
}

void hid_bpf_transport_close(int fd) {
    TransportState* t = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        auto it = g_registry.find(fd);
        if (it != g_registry.end()) {
            t = it->second;
            g_registry.erase(it);
        }
    }

    if (!t) {
        close(fd);
        return;
    }

    t->stop_flag.store(true, std::memory_order_relaxed);
    shutdown(t->sv[0], SHUT_RDWR);
    if (t->pump_thread.joinable()) {
        t->pump_thread.join();
    }

    ring_buffer__free(t->rb);
    close(t->ringbuf_map_fd);
    close(t->hidraw_fd);
    close(t->sv[0]);
    close(fd);

    run_udev_hid_bpf({"remove", t->sysfs_path});

    delete t;
}
