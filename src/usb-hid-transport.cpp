#include "usb-hid-transport.h"
#include "hid-driver-sysfs.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <fcntl.h>
#include <libusb-1.0/libusb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// DS4's report sizes top out at 78 bytes (Bluetooth); USB reports are
// smaller (64 bytes in, 32 out), but a shared 128-byte buffer keeps this
// simple and matches the buffer size main.cpp's own read() already uses.
constexpr size_t kMaxReportSize = 128;

struct TransportState {
    libusb_context* ctx = nullptr;
    libusb_device_handle* handle = nullptr;
    int interface_number = -1;
    uint8_t ep_in = 0;
    uint8_t ep_out = 0;
    int sv[2] = {-1, -1};
    std::thread pump_thread;
    std::atomic<bool> stop_flag{false};
    std::string hid_id;
    uint8_t bus_number = 0;
    uint8_t device_address = 0;
};

std::mutex g_registry_mutex;
std::unordered_map<int, TransportState*> g_registry;

// Finds the first HID-class interface (and its interrupt IN/OUT
// endpoints) on the device's active configuration, rather than
// hardcoding an interface/endpoint number -- DS4's exact layout should
// still be cross-checked against drivers/hid/hid-playstation.c before
// relying on this in the field, but discovering it from the descriptor
// is more robust than a hardcoded guess regardless.
bool find_hid_interface(libusb_device* dev, int& out_interface, uint8_t& out_ep_in, uint8_t& out_ep_out) {
    libusb_config_descriptor* config = nullptr;
    if (libusb_get_active_config_descriptor(dev, &config) != 0 || !config) {
        return false;
    }

    bool found = false;
    for (int i = 0; i < config->bNumInterfaces && !found; ++i) {
        const libusb_interface& iface = config->interface[i];
        for (int a = 0; a < iface.num_altsetting && !found; ++a) {
            const libusb_interface_descriptor& alt = iface.altsetting[a];
            if (alt.bInterfaceClass != LIBUSB_CLASS_HID) continue;

            uint8_t ep_in = 0, ep_out = 0;
            for (int e = 0; e < alt.bNumEndpoints; ++e) {
                const libusb_endpoint_descriptor& ep = alt.endpoint[e];
                if ((ep.bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_INTERRUPT) continue;
                if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                    ep_in = ep.bEndpointAddress;
                } else {
                    ep_out = ep.bEndpointAddress;
                }
            }
            if (ep_in != 0) {
                out_interface = alt.bInterfaceNumber;
                out_ep_in = ep_in;
                out_ep_out = ep_out; // DS4 output reports may also go via
                                     // control transfers if this is 0;
                                     // Phase A assumes a real interrupt
                                     // OUT endpoint exists, matching the
                                     // wired DS4's descriptor.
                found = true;
            }
        }
    }

    libusb_free_config_descriptor(config);
    return found;
}

// Checks whether t's target device is still present by bus/address --
// see pump_thread_fn's use for why this exists alongside the transfer
// error codes below.
bool device_still_present(TransportState* t) {
    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(t->ctx, &list);
    bool found = false;
    for (ssize_t i = 0; i < count && !found; ++i) {
        if (libusb_get_bus_number(list[i]) == t->bus_number &&
            libusb_get_device_address(list[i]) == t->device_address) {
            found = true;
        }
    }
    libusb_free_device_list(list, 1);
    return found;
}

void pump_thread_fn(TransportState* t) {
    uint8_t in_buf[kMaxReportSize];
    uint8_t out_buf[kMaxReportSize];
    int liveness_check_counter = 0;

    while (!t->stop_flag.load(std::memory_order_relaxed)) {
        // Outbound: forward at most one pending report per iteration --
        // LED/rumble writes are infrequent, so a non-blocking peek here
        // costs nothing while keeping this to a single thread.
        struct pollfd pfd;
        pfd.fd = t->sv[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(t->sv[0], out_buf, sizeof(out_buf));
            if (n > 0 && t->ep_out != 0) {
                int transferred = 0;
                libusb_interrupt_transfer(t->handle, t->ep_out, out_buf, (int)n, &transferred, 200);
            }
        }

        // Inbound: blocks up to 8ms (matches this daemon's own
        // steady-state poll granularity elsewhere) so stop_flag/outbound
        // writes are still checked promptly; returns immediately once
        // data actually arrives rather than waiting for the full timeout.
        int transferred = 0;
        int rc = libusb_interrupt_transfer(t->handle, t->ep_in, in_buf, sizeof(in_buf), &transferred, 8);
        if (rc == LIBUSB_SUCCESS && transferred > 0) {
            if (write(t->sv[0], in_buf, (size_t)transferred) < 0) {
                // Peer end closed (main thread already tore down its side) --
                // nothing left to deliver this report to; stop pumping.
                break;
            }
        } else if (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_IO) {
            std::cerr << "hid-unbind: USB transport lost the device (" << libusb_error_name(rc) << ")" << std::endl;
            break;
        }
        // LIBUSB_ERROR_TIMEOUT is the expected steady-state case -- loop.

        // Second, distinct belt-and-suspenders check: usb_hid_transport_open()
        // only verifies the claim held via libusb_kernel_driver_active()
        // *once*, ~300ms after claiming -- confirmed live (2026-08-10,
        // repeatedly, 100% reproduction) that this one-time check always
        // passes (no kernel driver active yet), but the kernel's usbhid
        // driver re-claims the interface *later* in the session anyway,
        // with the device still fully enumerated the whole time (so the
        // device_still_present() check above never fires either -- this
        // isn't a disconnect, the device never left). No corresponding
        // libusb_interrupt_transfer() error was ever observed at the
        // moment of reclaim in any test. The only way to catch this is
        // to keep asking the same question throughout the session, not
        // just once at open time.
        if (liveness_check_counter == 60) { // offset from the disconnect check below so they don't both land on the same tick
            int active_rc = libusb_kernel_driver_active(t->handle, t->interface_number);
            if (active_rc == 1) {
                std::cerr << "hid-unbind: interface " << t->interface_number
                          << " was reclaimed by the kernel's usbhid driver mid-session" << std::endl;
                break;
            }
        }

        // Belt-and-suspenders beyond the transfer error codes above:
        // confirmed live (2026-08-10) that a real physical disconnect
        // doesn't always surface as LIBUSB_ERROR_NO_DEVICE/IO from
        // libusb_interrupt_transfer() here -- across several rapid
        // unplug/replug cycles, this loop kept silently returning
        // LIBUSB_ERROR_TIMEOUT forever against a long-gone device
        // instead, with the pump thread (and therefore phy_fd, from
        // main.cpp's side) never noticing anything was wrong. That left
        // this daemon's own auto-scan permanently blocked from ever
        // claiming any of the fresh device instances that kept
        // appearing during the same test, since it only ever runs while
        // phy_fd < 0. Check roughly once a second (~125 iterations at
        // the 8ms poll granularity above) whether the exact bus/address
        // this transport opened is still enumerated at all.
        if (++liveness_check_counter >= 125) {
            liveness_check_counter = 0;
            if (!device_still_present(t)) {
                std::cerr << "hid-unbind: USB device bus " << (unsigned)t->bus_number
                          << " addr " << (unsigned)t->device_address
                          << " no longer enumerated (interrupt transfers kept timing out "
                             "instead of erroring)" << std::endl;
                break;
            }
        }
    }

    shutdown(t->sv[0], SHUT_RDWR);
}

} // namespace

int usb_hid_transport_open(const std::string& hid_id, uint16_t vid, uint16_t pid) {
    auto* t = new TransportState();
    t->hid_id = hid_id;

    if (libusb_init(&t->ctx) != 0) {
        std::cerr << "hid-unbind: libusb_init failed" << std::endl;
        delete t;
        write_hid_driver_sysfs("bind", hid_id);
        return -1;
    }

    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(t->ctx, &list);
    libusb_device* target = nullptr;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        if (desc.idVendor == vid && desc.idProduct == pid) {
            target = list[i];
            break;
        }
    }

    int interface_number = -1;
    uint8_t ep_in = 0, ep_out = 0;
    bool have_endpoints = target && find_hid_interface(target, interface_number, ep_in, ep_out);

    libusb_device_handle* handle = nullptr;
    uint8_t target_bus = 0, target_addr = 0;
    if (target && have_endpoints) {
        libusb_open(target, &handle);
        target_bus = libusb_get_bus_number(target);
        target_addr = libusb_get_device_address(target);
    }
    libusb_free_device_list(list, 1);

    if (!handle) {
        std::cerr << "hid-unbind: no matching/openable USB device for vid=0x"
                  << std::hex << vid << " pid=0x" << pid << std::dec << std::endl;
        libusb_exit(t->ctx);
        delete t;
        write_hid_driver_sysfs("bind", hid_id);
        return -1;
    }

    // The caller's HID-bus sysfs unbind (hid_id_unbind()) only detaches
    // the high-level driver (hid-playstation) from the hid_device it
    // creates -- it's a completely separate kernel binding from the
    // low-level "usbhid" driver that owns the raw USB *interface* and is
    // what actually creates that hid_device in the first place.
    // libusb_detach_kernel_driver() is what detaches usbhid specifically.
    //
    // On the BT->USB hot-swap path (see the "USB connection detected"
    // block in main.cpp), hid-playstation's own kernel-side probe retry
    // for this exact device can still be in flight here (it was failing
    // with -EEXIST -- "Duplicate device found for MAC address" -- while
    // Bluetooth held the MAC, and the kernel keeps retrying on its own
    // schedule once that clears). If that retry's *first* step --
    // usbhid re-claiming the raw interface -- lands between our detach
    // and our claim below, libusb_claim_interface() can still report
    // success (we got it first) while usbhid's own claim is still queued
    // right behind us and takes the interface back moments later, with
    // no libusb error at that moment to catch it -- confirmed live
    // (2026-08-10): our claim looked fine, then usbhid silently owned
    // the interface again with no error ever surfacing, only detectable
    // by directly asking libusb whether a kernel driver is active again
    // afterward. A plain USB-only connection (no prior Bluetooth bind
    // racing against it) never hits this -- retry a few times with a
    // settling delay rather than trying to out-guess the kernel's own
    // retry timing.
    constexpr int kMaxClaimAttempts = 4;
    bool claimed = false;
    for (int attempt = 1; attempt <= kMaxClaimAttempts && !claimed; ++attempt) {
        // LIBUSB_ERROR_NOT_FOUND (no kernel driver active) is expected
        // and not an error here.
        int detach_rc = libusb_detach_kernel_driver(handle, interface_number);
        if (detach_rc != 0 && detach_rc != LIBUSB_ERROR_NOT_FOUND && detach_rc != LIBUSB_ERROR_NOT_SUPPORTED) {
            std::cerr << "hid-unbind: libusb_detach_kernel_driver: " << libusb_error_name(detach_rc)
                      << " (attempt " << attempt << ")" << std::endl;
        }

        if (libusb_claim_interface(handle, interface_number) != 0) {
            std::cerr << "hid-unbind: libusb_claim_interface failed for interface " << interface_number
                      << " (attempt " << attempt << ")" << std::endl;
            usleep(200000 * attempt);
            continue;
        }

        // Give any in-flight kernel probe retry time to finish, then ask
        // libusb directly whether a kernel driver (usbhid) is active on
        // this interface again -- the one question sysfs alone can't
        // answer reliably here, since hid_id_unbind() already made the
        // *other* (HID-bus) layer look clean regardless.
        usleep(300000);
        int active_rc = libusb_kernel_driver_active(handle, interface_number);
        if (active_rc == 0) {
            claimed = true;
            break;
        }
        std::cerr << "hid-unbind: interface " << interface_number
                  << " was reclaimed by the kernel right after our claim (attempt "
                  << attempt << "/" << kMaxClaimAttempts << "), retrying..." << std::endl;
        libusb_release_interface(handle, interface_number);
        usleep(200000 * attempt);
    }

    if (!claimed) {
        std::cerr << "hid-unbind: giving up on interface " << interface_number
                  << " after " << kMaxClaimAttempts << " attempts -- kernel keeps reclaiming it" << std::endl;
        libusb_close(handle);
        libusb_exit(t->ctx);
        delete t;
        write_hid_driver_sysfs("bind", hid_id);
        return -1;
    }

    std::cerr << "hid-unbind: claimed interface " << interface_number
              << " ep_in=0x" << std::hex << (unsigned)ep_in
              << " ep_out=0x" << (unsigned)ep_out << std::dec << std::endl;

    int sv[2];
    // SOCK_SEQPACKET, not SOCK_DGRAM -- see usb-hid-transport.h's doc
    // comment: SOCK_DGRAM here never delivers POLLHUP/EOF to the peer
    // when this end closes (confirmed live), leaving phy_fd looking
    // permanently healthy after the device is actually gone.
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
        std::cerr << "hid-unbind: socketpair failed: " << strerror(errno) << std::endl;
        libusb_release_interface(handle, interface_number);
        libusb_close(handle);
        libusb_exit(t->ctx);
        delete t;
        write_hid_driver_sysfs("bind", hid_id);
        return -1;
    }

    int flags = fcntl(sv[1], F_GETFL, 0);
    fcntl(sv[1], F_SETFL, flags | O_NONBLOCK);

    t->handle = handle;
    t->interface_number = interface_number;
    t->ep_in = ep_in;
    t->ep_out = ep_out;
    t->sv[0] = sv[0];
    t->sv[1] = sv[1];
    t->bus_number = target_bus;
    t->device_address = target_addr;
    // Pump thread is started separately by usb_hid_transport_start() --
    // see its doc comment for why the interface is claimed here but left
    // idle until the caller says so.

    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        g_registry[sv[1]] = t;
    }

    return sv[1];
}

void usb_hid_transport_start(int fd) {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    auto it = g_registry.find(fd);
    if (it == g_registry.end()) return;
    TransportState* t = it->second;
    if (!t->pump_thread.joinable()) {
        t->pump_thread = std::thread(pump_thread_fn, t);
    }
}

void usb_hid_transport_close(int fd) {
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

    libusb_release_interface(t->handle, t->interface_number);
    int attach_rc = libusb_attach_kernel_driver(t->handle, t->interface_number);
    if (attach_rc != 0) {
        // libusb couldn't hand the interface back to the kernel driver
        // itself (common if it was never attached via libusb, e.g. after
        // a sysfs-only unbind) -- fall back to the same sysfs bind write
        // the legacy hot-swap path already uses, so the controller is
        // never left driverless.
        write_hid_driver_sysfs("bind", t->hid_id);
    }
    libusb_close(t->handle);
    libusb_exit(t->ctx);

    close(t->sv[0]);
    close(fd);
    delete t;
}
