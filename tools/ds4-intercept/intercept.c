/*
 * ds4-intercept.c — LD_PRELOAD diagnostic library for controller I/O tracing.
 *
 * Intercepts all possible communication paths between a game and controller
 * devices (hidraw, evdev, joystick, udev) and logs them to a file.
 *
 * Usage:
 *   LD_PRELOAD=/usr/lib/libds4-intercept.so <game>
 *
 * Log output: /tmp/ds4-intercept.log (or DS4_INTERCEPT_LOG env var)
 *
 * Intercepted paths:
 *   - open/openat  → detects opens of /dev/hidraw*, /dev/input/event*, /dev/input/js*
 *   - read/write   → logs data flowing to/from tracked fds (hex dump)
 *   - ioctl        → decodes evdev (EVIOCG*) and hidraw (HIDIOCG*) ioctls
 *   - poll/select  → logs when game polls tracked fds
 *   - close        → tracks fd lifecycle
 *   - libudev      → hooks udev_device and udev_enumerate functions
 */

// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) -- glibc feature-test macro, not user code
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <poll.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/sysmacros.h>
#include <linux/input.h>
#include <linux/hidraw.h>

/* ---------- configuration ---------- */

/*
 * Wine/Proton processes (esync/fsync) routinely hold hundreds of eventfds
 * open, so the actual game fd can land well past a small table's range.
 * With the old limit of 256, track_fd() would silently stop tracking and
 * every subsequent read/write/ioctl/poll on that fd went unlogged with no
 * indication anything was missed.
 */
#define MAX_TRACKED_FDS 4096
#define MAX_TRACKED_DIRS 64
#define HEX_DUMP_MAX    128   /* max bytes to hex-dump per read/write */

/* ---------- state ---------- */

typedef enum {
    DEV_NONE = 0,
    DEV_HIDRAW,
    DEV_EVDEV,
    DEV_JOYDEV,
    DEV_UHID,
    DEV_UINPUT,
    DEV_SYSFS,
} dev_type_t;

typedef struct {
    int        active;
    dev_type_t type;
    char       path[256];
} tracked_fd_t;

typedef struct {
    int   active;
    DIR  *dirp;
    char  path[256];
} tracked_dir_t;

static tracked_fd_t tracked_fds[MAX_TRACKED_FDS];
static tracked_dir_t tracked_dirs[MAX_TRACKED_DIRS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *logfile = NULL;
static int initialized = 0;
static pid_t self_pid = 0;
static char proc_name[64] = "?";

/* ---------- original function pointers ---------- */

typedef int   (*open_fn)(const char *, int, ...);
typedef int   (*openat_fn)(int, const char *, int, ...);
typedef ssize_t (*read_fn)(int, void *, size_t);
typedef ssize_t (*write_fn)(int, const void *, size_t);
typedef int   (*ioctl_fn)(int, unsigned long, ...);
typedef int   (*close_fn)(int);
typedef int   (*poll_fn)(struct pollfd *, nfds_t, int);
typedef int   (*dup_fn)(int);
typedef int   (*dup2_fn)(int, int);
typedef int   (*dup3_fn)(int, int, int);
typedef int   (*fcntl_fn)(int, int, ...);
typedef DIR  *(*opendir_fn)(const char *);
typedef struct dirent *(*readdir_fn)(DIR *);
typedef int   (*closedir_fn)(DIR *);
typedef int   (*execve_fn)(const char *, char *const[], char *const[]);

static open_fn   real_open;
static openat_fn real_openat;
static read_fn   real_read;
static write_fn  real_write;
static ioctl_fn  real_ioctl;
static close_fn  real_close;
static poll_fn   real_poll;
static dup_fn    real_dup;
static dup2_fn   real_dup2;
static dup3_fn   real_dup3;
static fcntl_fn  real_fcntl;
static opendir_fn  real_opendir;
static readdir_fn  real_readdir;
static closedir_fn real_closedir;
static execve_fn   real_execve;

/* ---------- helpers ---------- */

static void ensure_init(void) {
    if (initialized) return;
    initialized = 1;

    real_open   = (open_fn)  dlsym(RTLD_NEXT, "open");
    real_openat = (openat_fn)dlsym(RTLD_NEXT, "openat");
    real_read   = (read_fn)  dlsym(RTLD_NEXT, "read");
    real_write  = (write_fn) dlsym(RTLD_NEXT, "write");
    real_ioctl  = (ioctl_fn) dlsym(RTLD_NEXT, "ioctl");
    real_close  = (close_fn) dlsym(RTLD_NEXT, "close");
    real_poll   = (poll_fn)  dlsym(RTLD_NEXT, "poll");
    real_dup    = (dup_fn)   dlsym(RTLD_NEXT, "dup");
    real_dup2   = (dup2_fn)  dlsym(RTLD_NEXT, "dup2");
    real_dup3   = (dup3_fn)  dlsym(RTLD_NEXT, "dup3");
    real_fcntl  = (fcntl_fn) dlsym(RTLD_NEXT, "fcntl");
    real_opendir  = (opendir_fn) dlsym(RTLD_NEXT, "opendir");
    real_readdir  = (readdir_fn) dlsym(RTLD_NEXT, "readdir");
    real_closedir = (closedir_fn)dlsym(RTLD_NEXT, "closedir");
    real_execve   = (execve_fn)  dlsym(RTLD_NEXT, "execve");

    self_pid = getpid();
    strncpy(proc_name, program_invocation_short_name, sizeof(proc_name) - 1);

    const char *path = getenv("DS4_INTERCEPT_LOG");
    if (!path) path = "/tmp/ds4-intercept.log";
    logfile = fopen(path, "a");
    if (!logfile) logfile = stderr;
    setlinebuf(logfile);
}

static void log_msg(const char *fmt, ...) {
    ensure_init();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    pthread_mutex_lock(&lock);
    (void)fprintf(logfile, "[%5ld.%03ld] [pid=%d %s tid=%ld] ",
            (long)ts.tv_sec % 100000, ts.tv_nsec / 1000000,
            self_pid, proc_name, (long)gettid());
    va_list ap;
    va_start(ap, fmt);
    (void)vfprintf(logfile, fmt, ap);
    va_end(ap);
    (void)fputc('\n', logfile);
    pthread_mutex_unlock(&lock);
}

static void hex_dump(const char *label, const void *data, size_t len) {
    ensure_init();
    size_t show = (len > HEX_DUMP_MAX) ? HEX_DUMP_MAX : len;
    char buf[HEX_DUMP_MAX * 3 + 16];
    int pos = 0;
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < show; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x ", p[i]);
    }
    if (len > show) {
        (void)snprintf(buf + pos, sizeof(buf) - pos, "... (+%zu more)", len - show);
    }
    log_msg("  %s (%zu bytes): %s", label, len, buf);
}

static dev_type_t classify_path(const char *path) {
    if (!path) return DEV_NONE;
    if (strstr(path, "/dev/hidraw"))       return DEV_HIDRAW;
    if (strstr(path, "/dev/input/event"))  return DEV_EVDEV;
    if (strstr(path, "/dev/input/js"))     return DEV_JOYDEV;
    if (!strcmp(path, "/dev/uhid"))        return DEV_UHID;
    if (!strcmp(path, "/dev/uinput") || !strcmp(path, "/dev/input/uinput"))
                                            return DEV_UINPUT;
    /*
     * Wine's bus_udev.c and libudev enumerate/identify devices by reading
     * sysfs directly (uevent, idVendor/idProduct, report_descriptor, ...)
     * rather than only opening the device node. Without this, any
     * enumeration failure happening purely at the sysfs level is invisible.
     */
    if (strstr(path, "/sys/class/hidraw"))      return DEV_SYSFS;
    if (strstr(path, "/sys/class/input"))       return DEV_SYSFS;
    if (strstr(path, "/sys/bus/hid/devices"))   return DEV_SYSFS;
    if (strstr(path, "/sys/bus/usb/devices"))   return DEV_SYSFS;
    return DEV_NONE;
}

static const char *dev_type_str(dev_type_t t) {
    switch (t) {
        case DEV_HIDRAW: return "hidraw";
        case DEV_EVDEV:  return "evdev";
        case DEV_JOYDEV: return "joydev";
        case DEV_UHID:   return "uhid";
        case DEV_UINPUT: return "uinput";
        case DEV_SYSFS:  return "sysfs";
        default:         return "unknown";
    }
}

static void track_fd(int fd, const char *path, dev_type_t type) {
    if (fd < 0 || fd >= MAX_TRACKED_FDS) {
        log_msg("WARNING: fd=%d for %s exceeds MAX_TRACKED_FDS=%d, tracking lost",
                fd, path, MAX_TRACKED_FDS);
        return;
    }
    pthread_mutex_lock(&lock);
    tracked_fds[fd].active = 1;
    tracked_fds[fd].type = type;
    strncpy(tracked_fds[fd].path, path, sizeof(tracked_fds[fd].path) - 1);
    tracked_fds[fd].path[sizeof(tracked_fds[fd].path) - 1] = '\0';
    pthread_mutex_unlock(&lock);
}

static tracked_fd_t *get_tracked(int fd) {
    if (fd < 0 || fd >= MAX_TRACKED_FDS) return NULL;
    if (!tracked_fds[fd].active) return NULL;
    return &tracked_fds[fd];
}

static void untrack_fd(int fd) {
    if (fd < 0 || fd >= MAX_TRACKED_FDS) return;
    pthread_mutex_lock(&lock);
    tracked_fds[fd].active = 0;
    pthread_mutex_unlock(&lock);
}

/* ---------- directory tracking (enumeration visibility) ---------- */

static int is_interesting_dir(const char *path) {
    if (!path) return 0;
    return strstr(path, "/dev/input") != NULL ||
           !strcmp(path, "/dev") ||
           strstr(path, "/sys/class/hidraw") != NULL ||
           strstr(path, "/sys/class/input") != NULL ||
           strstr(path, "/sys/bus/hid/devices") != NULL ||
           strstr(path, "/sys/bus/usb/devices") != NULL;
}

static void track_dir(DIR *dirp, const char *path) {
    if (!dirp) return;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_TRACKED_DIRS; i++) {
        if (!tracked_dirs[i].active) {
            tracked_dirs[i].active = 1;
            tracked_dirs[i].dirp = dirp;
            strncpy(tracked_dirs[i].path, path, sizeof(tracked_dirs[i].path) - 1);
            tracked_dirs[i].path[sizeof(tracked_dirs[i].path) - 1] = '\0';
            break;
        }
    }
    pthread_mutex_unlock(&lock);
}

static const char *get_tracked_dir(DIR *dirp) {
    if (!dirp) return NULL;
    for (int i = 0; i < MAX_TRACKED_DIRS; i++) {
        if (tracked_dirs[i].active && tracked_dirs[i].dirp == dirp)
            return tracked_dirs[i].path;
    }
    return NULL;
}

static void untrack_dir(DIR *dirp) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_TRACKED_DIRS; i++) {
        if (tracked_dirs[i].active && tracked_dirs[i].dirp == dirp) {
            tracked_dirs[i].active = 0;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
}

/* ---------- evdev event decoding ---------- */

static const char *evdev_type_name(unsigned short type) {
    switch (type) {
        case EV_SYN: return "EV_SYN";
        case EV_KEY: return "EV_KEY";
        case EV_REL: return "EV_REL";
        case EV_ABS: return "EV_ABS";
        case EV_MSC: return "EV_MSC";
        case EV_FF:  return "EV_FF";
        case EV_FF_STATUS: return "EV_FF_STATUS";
        default:     return "EV_?";
    }
}

/* ---------- ioctl name decoding ---------- */

static const char *decode_evdev_ioctl(unsigned long request) {
    /* EVIOCG* family — _IOR('E', ...) */
    switch (request) {
        case EVIOCGVERSION:  return "EVIOCGVERSION";
        case EVIOCGID:       return "EVIOCGID";
        case EVIOCGREP:      return "EVIOCGREP";
        case EVIOCGKEYCODE:  return "EVIOCGKEYCODE";
        case EVIOCGEFFECTS:  return "EVIOCGEFFECTS";
#ifdef EVIOCGMASK
        case EVIOCGMASK:     return "EVIOCGMASK";
#endif
        default: break;
    }

    /* EVIOCGNAME(len), EVIOCGPHYS(len), EVIOCGUNIQ(len) are variable-size */
    unsigned int nr = _IOC_NR(request);
    unsigned int type = _IOC_TYPE(request);
    unsigned int dir = _IOC_DIR(request);
    unsigned int size = _IOC_SIZE(request);

    if (type == 'E') {
        static char buf[64];
        switch (nr) {
            case 0x06: (void)snprintf(buf, sizeof(buf), "EVIOCGNAME(%u)", size); return buf;
            case 0x07: (void)snprintf(buf, sizeof(buf), "EVIOCGPHYS(%u)", size); return buf;
            case 0x08: (void)snprintf(buf, sizeof(buf), "EVIOCGUNIQ(%u)", size); return buf;
            case 0x09: (void)snprintf(buf, sizeof(buf), "EVIOCGPROP(%u)", size); return buf;
            case 0x90: (void)snprintf(buf, sizeof(buf), "EVIOCGRAB(%s)", (dir & _IOC_WRITE) ? "grab" : "release"); return buf;
            default:
                /* EVIOCGBIT(ev, len) = _IOC(_IOC_READ, 'E', 0x20+ev, len) */
                if (nr >= 0x20 && nr < 0x40) {
                    (void)snprintf(buf, sizeof(buf), "EVIOCGBIT(type=%u, len=%u)", nr - 0x20, size);
                    return buf;
                }
                /* EVIOCGABS(axis) = _IOR('E', 0x40+axis, struct input_absinfo) */
                if (nr >= 0x40 && nr < 0x80) {
                    (void)snprintf(buf, sizeof(buf), "EVIOCGABS(axis=%u)", nr - 0x40);
                    return buf;
                }
                /* EVIOCGKEY, etc */
                (void)snprintf(buf, sizeof(buf), "EVIOC_0x%02x(sz=%u)", nr, size);
                return buf;
        }
    }

    return NULL;
}

static const char *decode_hidraw_ioctl(unsigned long request) {
    switch (request) {
        case HIDIOCGRDESCSIZE: return "HIDIOCGRDESCSIZE";
        case HIDIOCGRDESC:     return "HIDIOCGRDESC";
        case HIDIOCGRAWINFO:   return "HIDIOCGRAWINFO";
        default: break;
    }

    unsigned int type = _IOC_TYPE(request);
    unsigned int nr = _IOC_NR(request);
    unsigned int size = _IOC_SIZE(request);

    if (type == 'H') {
        static char buf[64];
        switch (nr) {
            case 0x04: (void)snprintf(buf, sizeof(buf), "HIDIOCGRAWNAME(%u)", size); return buf;
            case 0x05: (void)snprintf(buf, sizeof(buf), "HIDIOCGRAWPHYS(%u)", size); return buf;
            case 0x06: (void)snprintf(buf, sizeof(buf), "HIDIOCSFEATURE(%u)", size); return buf;
            case 0x07: (void)snprintf(buf, sizeof(buf), "HIDIOCGFEATURE(%u)", size); return buf;
            case 0x08: (void)snprintf(buf, sizeof(buf), "HIDIOCGRAWUNIQ(%u)", size); return buf;
            default:
                (void)snprintf(buf, sizeof(buf), "HIDRAW_0x%02x(sz=%u)", nr, size);
                return buf;
        }
    }

    return NULL;
}

static const char *decode_ioctl(unsigned long request, dev_type_t type) {
    const char *name = NULL;

    if (type == DEV_EVDEV || type == DEV_JOYDEV) {
        name = decode_evdev_ioctl(request);
    }
    if (!name && type == DEV_HIDRAW) {
        name = decode_hidraw_ioctl(request);
    }

    if (!name) {
        static char buf[32];
        (void)snprintf(buf, sizeof(buf), "ioctl(0x%08lx)", request);
        name = buf;
    }
    return name;
}

/* ---------- hooked functions ---------- */

static int handle_open(const char *pathname, int flags, mode_t mode) {
    ensure_init();
    dev_type_t type = classify_path(pathname);
    int fd = real_open(pathname, flags, mode);

    if (type != DEV_NONE) {
        if (fd >= 0) {
            track_fd(fd, pathname, type);
            log_msg("OPEN %s → fd=%d (type=%s, flags=0x%x)",
                    pathname, fd, dev_type_str(type), flags);
        } else {
            log_msg("OPEN %s → FAILED errno=%d (%s) (type=%s, flags=0x%x)",
                    pathname, errno, strerror(errno), dev_type_str(type), flags);
        }
    }
    return fd;
}

int open(const char *pathname, int flags, ...) {
    ensure_init();
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return handle_open(pathname, flags, mode);
}

int open64(const char *pathname, int flags, ...) {
    ensure_init();
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return handle_open(pathname, flags, mode);
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    ensure_init();
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    dev_type_t type = classify_path(pathname);
    int fd = real_openat(dirfd, pathname, flags, mode);

    if (type != DEV_NONE) {
        if (fd >= 0) {
            track_fd(fd, pathname, type);
            log_msg("OPENAT %s → fd=%d (type=%s, flags=0x%x)",
                    pathname, fd, dev_type_str(type), flags);
        } else {
            log_msg("OPENAT %s → FAILED errno=%d (%s) (type=%s)",
                    pathname, errno, strerror(errno), dev_type_str(type));
        }
    }
    return fd;
}

int openat64(int dirfd, const char *pathname, int flags, ...) {
    ensure_init();
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    dev_type_t type = classify_path(pathname);
    int fd = real_openat(dirfd, pathname, flags, mode);

    if (type != DEV_NONE) {
        if (fd >= 0) {
            track_fd(fd, pathname, type);
            log_msg("OPENAT64 %s → fd=%d (type=%s)", pathname, fd, dev_type_str(type));
        } else {
            log_msg("OPENAT64 %s → FAILED errno=%d (%s)", pathname, errno, strerror(errno));
        }
    }
    return fd;
}

ssize_t read(int fd, void *buf, size_t count) {
    ensure_init();
    ssize_t ret = real_read(fd, buf, count);
    tracked_fd_t *t = get_tracked(fd);
    if (t && ret > 0) {
        log_msg("READ fd=%d (%s %s) → %zd bytes", fd, dev_type_str(t->type), t->path, ret);
        hex_dump("data", buf, ret);
        if (t->type == DEV_EVDEV && (size_t)ret % sizeof(struct input_event) == 0) {
            size_t n = (size_t)ret / sizeof(struct input_event);
            struct input_event *evs = (struct input_event *)buf;
            for (size_t i = 0; i < n; i++) {
                log_msg("  event[%zu]: type=%s(%u) code=%u value=%d",
                        i, evdev_type_name(evs[i].type), evs[i].type,
                        evs[i].code, evs[i].value);
            }
        }
    } else if (t && ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        log_msg("READ fd=%d (%s %s) → ERROR errno=%d (%s)",
                fd, dev_type_str(t->type), t->path, errno, strerror(errno));
    }
    return ret;
}

ssize_t write(int fd, const void *buf, size_t count) {
    ensure_init();
    tracked_fd_t *t = get_tracked(fd);
    if (t) {
        log_msg("WRITE fd=%d (%s %s) %zu bytes", fd, dev_type_str(t->type), t->path, count);
        hex_dump("data", buf, count);
    }
    return real_write(fd, buf, count);
}

int ioctl(int fd, unsigned long request, ...) {
    ensure_init();
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    tracked_fd_t *t = get_tracked(fd);

    /*
     * HIDIOCGFEATURE requires the caller to pre-fill arg[0] with the report
     * ID being requested, and HIDIOCSFEATURE/EVIOCSFF send data *to* the
     * device. That request-side payload is only visible before the call —
     * log it here, since it's the only way to know e.g. which DS4 feature
     * report (calibration 0x02 vs MAC/pairing 0x10/0x12 vs version 0x31/0xa3)
     * a game/SDL2/Wine actually asked for.
     */
    if (t && arg) {
        unsigned int req_type = _IOC_TYPE(request);
        unsigned int req_nr   = _IOC_NR(request);
        if (req_type == 'H' && req_nr == 0x07) { /* HIDIOCGFEATURE */
            log_msg("  → requesting feature report id=0x%02x", *(unsigned char *)arg);
        } else if (req_type == 'H' && req_nr == 0x06) { /* HIDIOCSFEATURE */
            hex_dump("SET feature report (request)", arg, _IOC_SIZE(request));
        } else if (req_type == 'E' && req_nr == 0x80) { /* EVIOCSFF */
            log_msg("  → uploading force-feedback effect");
        }
    }

    int ret = real_ioctl(fd, request, arg);

    if (t) {
        const char *name = decode_ioctl(request, t->type);
        log_msg("IOCTL fd=%d (%s %s) %s → ret=%d%s",
                fd, dev_type_str(t->type), t->path, name, ret,
                (ret < 0) ? strerror(errno) : "");

        /*
         * Log specific ioctl results. NOTE: fixed-size "get struct" ioctls
         * (EVIOCGID, HIDIOCGRAWINFO, HIDIOCGRDESCSIZE, EVIOCGABS) return 0
         * on success, but variable-length "get data" ioctls (EVIOCGNAME/
         * PHYS/UNIQ, HIDIOCGRAWNAME/PHYS/UNIQ, EVIOCGBIT, HIDIOCGFEATURE)
         * return the number of bytes copied instead — which is never 0 for
         * a non-empty result. Gating this whole block on ret==0 meant every
         * one of those variable-length results, including the feature-report
         * payload, was silently never decoded. Use ret>=0 instead, and use
         * the actual returned length (not the requested buffer size) where
         * it's meaningful.
         */
        if (ret >= 0) {
            if (request == EVIOCGID && arg) {
                struct input_id *id = (struct input_id *)arg;
                log_msg("  → bustype=0x%04x vendor=0x%04x product=0x%04x version=0x%04x",
                        id->bustype, id->vendor, id->product, id->version);
            }
            if (request == HIDIOCGRAWINFO && arg) {
                struct hidraw_devinfo *info = (struct hidraw_devinfo *)arg;
                log_msg("  → bustype=%u vendor=0x%04hx product=0x%04hx",
                        info->bustype, info->vendor, info->product);
            }
            if (request == HIDIOCGRDESCSIZE && arg) {
                int *sz = (int *)arg;
                log_msg("  → descriptor size=%d", *sz);
            }
            /* Variable-size name/phys/uniq ioctls */
            unsigned int nr = _IOC_NR(request);
            unsigned int ioc_type = _IOC_TYPE(request);
            if (ioc_type == 'E' && (nr == 0x06 || nr == 0x07 || nr == 0x08) && arg) {
                log_msg("  → \"%s\"", (char *)arg);
            }
            if (ioc_type == 'H' && (nr == 0x04 || nr == 0x05 || nr == 0x08) && arg) {
                log_msg("  → \"%s\"", (char *)arg);
            }
            /* HIDIOCGRDESC */
            if (request == HIDIOCGRDESC && arg) {
                struct hidraw_report_descriptor *desc = (struct hidraw_report_descriptor *)arg;
                hex_dump("HID descriptor", desc->value,
                         desc->size > 256 ? 256 : desc->size);
            }
            /* EVIOCGBIT results */
            if (ioc_type == 'E' && nr >= 0x20 && nr < 0x40 && arg) {
                unsigned int sz = _IOC_SIZE(request);
                hex_dump("capability bits", arg, sz);
            }
            /* EVIOCGABS results */
            if (ioc_type == 'E' && nr >= 0x40 && nr < 0x80 && arg) {
                struct input_absinfo *abs = (struct input_absinfo *)arg;
                log_msg("  → value=%d min=%d max=%d fuzz=%d flat=%d",
                        abs->value, abs->minimum, abs->maximum, abs->fuzz, abs->flat);
            }
            /* HIDIOCGFEATURE reply: ret is the actual byte count returned,
             * which is what matters, not the buffer size that was requested. */
            if (ioc_type == 'H' && nr == 0x07 && arg && ret > 0) {
                hex_dump("feature report (reply)", arg, (size_t)ret);
            }
        }
    }

    return ret;
}

int close(int fd) {
    ensure_init();
    tracked_fd_t *t = get_tracked(fd);
    if (t) {
        log_msg("CLOSE fd=%d (%s %s)", fd, dev_type_str(t->type), t->path);
        untrack_fd(fd);
    }
    return real_close(fd);
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    ensure_init();
    int ret = real_poll(fds, nfds, timeout);

    if (ret > 0) {
        for (nfds_t i = 0; i < nfds; i++) {
            if (fds[i].revents == 0) continue;
            tracked_fd_t *t = get_tracked(fds[i].fd);
            if (t) {
                log_msg("POLL fd=%d (%s %s) revents=0x%04x%s%s%s%s",
                        fds[i].fd, dev_type_str(t->type), t->path, fds[i].revents,
                        (fds[i].revents & POLLIN)  ? " IN"  : "",
                        (fds[i].revents & POLLOUT) ? " OUT" : "",
                        (fds[i].revents & POLLERR) ? " ERR" : "",
                        (fds[i].revents & POLLHUP) ? " HUP" : "");
            }
        }
    }

    return ret;
}

/*
 * dup/dup2/dup3/fcntl(F_DUPFD*) hooks: without these, a game that dup()s a
 * tracked hidraw/evdev fd into its own event loop (common pattern) causes
 * the duplicate to silently fall out of tracking — all subsequent read/
 * write/ioctl/poll traffic on it goes unlogged with no indication anything
 * was missed.
 */
int dup(int oldfd) {
    ensure_init();
    int newfd = real_dup(oldfd);
    if (newfd >= 0) {
        tracked_fd_t *t = get_tracked(oldfd);
        if (t) {
            log_msg("DUP fd=%d → fd=%d (%s %s)", oldfd, newfd, dev_type_str(t->type), t->path);
            track_fd(newfd, t->path, t->type);
        }
    }
    return newfd;
}

int dup2(int oldfd, int newfd) {
    ensure_init();
    int ret = real_dup2(oldfd, newfd);
    if (ret >= 0) {
        tracked_fd_t *t = get_tracked(oldfd);
        if (t) {
            log_msg("DUP2 fd=%d → fd=%d (%s %s)", oldfd, newfd, dev_type_str(t->type), t->path);
            track_fd(newfd, t->path, t->type);
        } else {
            untrack_fd(newfd);
        }
    }
    return ret;
}

int dup3(int oldfd, int newfd, int flags) {
    ensure_init();
    int ret = real_dup3(oldfd, newfd, flags);
    if (ret >= 0) {
        tracked_fd_t *t = get_tracked(oldfd);
        if (t) {
            log_msg("DUP3 fd=%d → fd=%d (%s %s)", oldfd, newfd, dev_type_str(t->type), t->path);
            track_fd(newfd, t->path, t->type);
        } else {
            untrack_fd(newfd);
        }
    }
    return ret;
}

int fcntl(int fd, int cmd, ...) {
    ensure_init();
    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    int ret = real_fcntl(fd, cmd, arg);

    if (ret >= 0 && (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC)) {
        tracked_fd_t *t = get_tracked(fd);
        if (t) {
            log_msg("FCNTL(F_DUPFD%s) fd=%d → fd=%d (%s %s)",
                    (cmd == F_DUPFD_CLOEXEC) ? "_CLOEXEC" : "",
                    fd, ret, dev_type_str(t->type), t->path);
            track_fd(ret, t->path, t->type);
        }
    }
    return ret;
}

/*
 * opendir/readdir/closedir on /dev/input, /dev, /sys/class/hidraw,
 * /sys/class/input, /sys/bus/hid|usb/devices — directory-scan enumeration
 * is often the very first step a game/SDL2/Wine takes to discover
 * controllers, and none of the open()/ioctl() hooks above see it at all.
 */
DIR *opendir(const char *name) {
    ensure_init();
    DIR *d = real_opendir(name);
    if (d && is_interesting_dir(name)) {
        track_dir(d, name);
        log_msg("OPENDIR %s", name);
    }
    return d;
}

struct dirent *readdir(DIR *dirp) {
    ensure_init();
    struct dirent *e = real_readdir(dirp);
    if (e) {
        const char *path = get_tracked_dir(dirp);
        if (path) {
            log_msg("READDIR %s → %s", path, e->d_name);
        }
    }
    return e;
}

int closedir(DIR *dirp) {
    ensure_init();
    if (get_tracked_dir(dirp)) {
        log_msg("CLOSEDIR");
        untrack_dir(dirp);
    }
    return real_closedir(dirp);
}

/*
 * execve: LD_PRELOAD (and DS4_INTERCEPT_LOG, so all children log to the
 * same file) is inherited across exec by default, and a Proton/Wine launch
 * is a whole tree of processes (steam → reaper → proton → wineserver,
 * services.exe/plugplay.exe, explorer.exe, the game's .exe...). Logging
 * every exec makes it possible to see the full chain and confirm the
 * interceptor actually loaded into the process that matters (the game
 * itself, or whichever Wine process performs bus_udev enumeration) rather
 * than only the launcher scripts around it.
 */
int execve(const char *pathname, char *const argv[], char *const envp[]) {
    ensure_init();
    log_msg("EXECVE %s", pathname);
    return real_execve(pathname, argv, envp);
}

/*
 * NOTE: libudev hooks (udev_device_get_parent_with_subsystem_devtype, etc.)
 * are intentionally NOT included. Exporting those symbols via LD_PRELOAD
 * shadows Steam's bundled libudev and causes SDL2 to misidentify controllers.
 * The syscall-level hooks above (open, ioctl, read, write) capture all the
 * information needed for diagnosis — including HIDIOCGRAWINFO (bus type),
 * EVIOCGID (vendor/product), and HIDIOCGFEATURE (feature reports).
 */


/* ---------- constructor ---------- */

static const char *interesting_env_vars[] = {
    "SDL_JOYSTICK_HIDAPI", "SDL_JOYSTICK_HIDAPI_PS4", "SDL_JOYSTICK_HIDAPI_PS4_RUMBLE",
    "SDL_JOYSTICK_DEVICE", "SDL_GAMECONTROLLERCONFIG", "SDL_GAMECONTROLLER_IGNORE_DEVICES",
    "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", "SDL_JOYSTICK_HIDAPI_JOY_CONS",
    "WINEDLLOVERRIDES", "WINEPREFIX", "WINEESYNC", "WINEFSYNC", "WINEDEBUG",
    "PROTON_LOG", "PROTON_USE_WINED3D", "SteamGameId", "SteamAppId",
    "STEAM_COMPAT_DATA_PATH", "STEAM_COMPAT_CLIENT_INSTALL_PATH",
    "LD_PRELOAD", "DS4_INTERCEPT_LOG",
    NULL
};

__attribute__((constructor))
static void intercept_init(void) {
    ensure_init();
    log_msg("=== ds4-intercept loaded into pid=%d (%s) ===", getpid(), program_invocation_short_name);
    for (int i = 0; interesting_env_vars[i]; i++) {
        const char *v = getenv(interesting_env_vars[i]);
        if (v) log_msg("  env %s=%s", interesting_env_vars[i], v);
    }
}

__attribute__((destructor))
static void intercept_fini(void) {
    if (logfile && logfile != stderr) {
        log_msg("=== ds4-intercept unloading ===");
        (void)fclose(logfile);
        logfile = NULL;
    }
}
