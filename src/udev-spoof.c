#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

struct udev_device;

static struct udev_device *(*real_get_parent)(struct udev_device *, const char *, const char *) = NULL;
static const char *(*real_get_sysattr)(struct udev_device *, const char *) = NULL;
static const char *(*real_get_property)(struct udev_device *, const char *) = NULL;
static const char *(*real_get_subsystem)(struct udev_device *) = NULL;

static void init_functions() {
    if (!real_get_parent) {
        void *handle = dlopen("libudev.so.1", RTLD_LAZY);
        if (!handle) handle = dlopen("libudev.so", RTLD_LAZY);
        if (!handle) {
            fprintf(stderr, "[udev-spoof] ERROR: Failed to load libudev.so.1 or libudev.so!\n");
            return;
        }
        real_get_parent = dlsym(handle, "udev_device_get_parent_with_subsystem_devtype");
        real_get_sysattr = dlsym(handle, "udev_device_get_sysattr_value");
        real_get_property = dlsym(handle, "udev_device_get_property_value");
        real_get_subsystem = dlsym(handle, "udev_device_get_subsystem");
        fprintf(stderr, "[udev-spoof] Library initialized: parent=%p, sysattr=%p, property=%p, subsystem=%p\n", 
                real_get_parent, real_get_sysattr, real_get_property, real_get_subsystem);
    }
}

static int is_virtual_sony_controller(struct udev_device *dev) {
    init_functions();
    if (!dev || !real_get_property) return 0;
    
    struct udev_device *clean_dev = (struct udev_device *)((uintptr_t)dev & ~1);
    const char *vid = real_get_property(clean_dev, "ID_VENDOR_ID");
    const char *pid = real_get_property(clean_dev, "ID_MODEL_ID");
    const char *devpath = real_get_property(clean_dev, "DEVPATH");
    
    int is_virtual = devpath && strstr(devpath, "/devices/virtual/misc/uhid/") != NULL;
    
    if (is_virtual && vid && !strcmp(vid, "054c")) {
        if (pid && (!strcmp(pid, "05c4") || !strcmp(pid, "0ce6"))) {
            return 1;
        }
    }
    return 0;
}

struct udev_device *udev_device_get_parent_with_subsystem_devtype(struct udev_device *udev_device, const char *subsystem, const char *devtype) {
    init_functions();
    
    if (subsystem && !strcmp(subsystem, "usb") && is_virtual_sony_controller(udev_device)) {
        fprintf(stderr, "[udev-spoof] Spoofing parent query for virtual Sony controller. Returning marked fake parent.\n");
        return (struct udev_device *)((uintptr_t)udev_device | 1);
    }
    
    if ((uintptr_t)udev_device & 1) {
        struct udev_device *real_dev = (struct udev_device *)((uintptr_t)udev_device & ~1);
        if (real_get_parent) {
            return real_get_parent(real_dev, subsystem, devtype);
        }
        return NULL;
    }
    
    if (real_get_parent) {
        return real_get_parent(udev_device, subsystem, devtype);
    }
    return NULL;
}

const char *udev_device_get_subsystem(struct udev_device *udev_device) {
    init_functions();
    
    if ((uintptr_t)udev_device & 1) {
        fprintf(stderr, "[udev-spoof] Spoofing subsystem query for fake parent: usb\n");
        return "usb";
    }
    
    if (real_get_subsystem) {
        return real_get_subsystem(udev_device);
    }
    return NULL;
}

const char *udev_device_get_sysattr_value(struct udev_device *udev_device, const char *sysattr) {
    init_functions();
    
    if ((uintptr_t)udev_device & 1) {
        struct udev_device *real_dev = (struct udev_device *)((uintptr_t)udev_device & ~1);
        const char *pid = real_get_property(real_dev, "ID_MODEL_ID");
        int is_dualsense = pid && !strcmp(pid, "0ce6");
        
        fprintf(stderr, "[udev-spoof] Spoofing sysattr query on fake parent: %s\n", sysattr);
        
        if (!strcmp(sysattr, "manufacturer")) {
            return is_dualsense ? "Sony Interactive Entertainment" : "Sony Computer Entertainment";
        }
        if (!strcmp(sysattr, "product")) {
            return is_dualsense ? "DualSense Wireless Controller" : "Wireless Controller";
        }
        if (!strcmp(sysattr, "serial")) {
            return "74:e7:d6:3a:47:e8";
        }
        if (!strcmp(sysattr, "uevent")) {
            const char *real_uevent = real_get_sysattr ? real_get_sysattr(real_dev, sysattr) : "";
            static char spoofed_uevent[2048];
            snprintf(spoofed_uevent, sizeof(spoofed_uevent), "%s\nPRODUCT=054c/05c4/0100\n", real_uevent);
            return spoofed_uevent;
        }
    }
    
    if (real_get_sysattr) {
        struct udev_device *clean_dev = (struct udev_device *)((uintptr_t)udev_device & ~1);
        return real_get_sysattr(clean_dev, sysattr);
    }
    return NULL;
}
