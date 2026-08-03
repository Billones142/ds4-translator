#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

struct udev_device;

static struct udev_device *(*real_get_parent)(struct udev_device *, const char *, const char *) = NULL;
static const char *(*real_get_sysattr)(struct udev_device *, const char *) = NULL;
static const char *(*real_get_property)(struct udev_device *, const char *) = NULL;

static void init_functions() {
    if (!real_get_parent) {
        real_get_parent = dlsym(RTLD_NEXT, "udev_device_get_parent_with_subsystem_devtype");
        real_get_sysattr = dlsym(RTLD_NEXT, "udev_device_get_sysattr_value");
        real_get_property = dlsym(RTLD_NEXT, "udev_device_get_property_value");
    }
}

static int is_virtual_sony_controller(struct udev_device *dev) {
    init_functions();
    if (!dev || !real_get_property) return 0;
    
    const char *vid = real_get_property(dev, "ID_VENDOR_ID");
    const char *pid = real_get_property(dev, "ID_MODEL_ID");
    const char *devpath = real_get_property(dev, "DEVPATH");
    
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
        return udev_device;
    }
    
    if (real_get_parent) {
        return real_get_parent(udev_device, subsystem, devtype);
    }
    return NULL;
}

const char *udev_device_get_sysattr_value(struct udev_device *udev_device, const char *sysattr) {
    init_functions();
    
    if (sysattr && is_virtual_sony_controller(udev_device)) {
        const char *pid = real_get_property(udev_device, "ID_MODEL_ID");
        int is_dualsense = pid && !strcmp(pid, "0ce6");
        
        if (!strcmp(sysattr, "manufacturer")) {
            return is_dualsense ? "Sony Interactive Entertainment" : "Sony Computer Entertainment";
        }
        if (!strcmp(sysattr, "product")) {
            return is_dualsense ? "DualSense Wireless Controller" : "Wireless Controller";
        }
        if (!strcmp(sysattr, "serial")) {
            return "74:e7:d6:3a:47:e8";
        }
    }
    
    if (real_get_sysattr) {
        return real_get_sysattr(udev_device, sysattr);
    }
    return NULL;
}
