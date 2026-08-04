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
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/sysmacros.h>
#include <linux/input.h>
#include <linux/hidraw.h>

/* ---------- configuration ---------- */

#define MAX_TRACKED_FDS 256
#define HEX_DUMP_MAX    128   /* max bytes to hex-dump per read/write */

/* ---------- state ---------- */

typedef enum {
    DEV_NONE = 0,
    DEV_HIDRAW,
    DEV_EVDEV,
    DEV_JOYDEV,
} dev_type_t;

typedef struct {
    int        active;
    dev_type_t type;
    char       path[256];
} tracked_fd_t;

static tracked_fd_t tracked_fds[MAX_TRACKED_FDS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *logfile = NULL;
static int initialized = 0;

/* ---------- original function pointers ---------- */

typedef int   (*open_fn)(const char *, int, ...);
typedef int   (*openat_fn)(int, const char *, int, ...);
typedef ssize_t (*read_fn)(int, void *, size_t);
typedef ssize_t (*write_fn)(int, const void *, size_t);
typedef int   (*ioctl_fn)(int, unsigned long, ...);
typedef int   (*close_fn)(int);
typedef int   (*poll_fn)(struct pollfd *, nfds_t, int);

static open_fn   real_open;
static openat_fn real_openat;
static read_fn   real_read;
static write_fn  real_write;
static ioctl_fn  real_ioctl;
static close_fn  real_close;
static poll_fn   real_poll;

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
    fprintf(logfile, "[%5ld.%03ld] [tid=%ld] ",
            (long)ts.tv_sec % 100000, ts.tv_nsec / 1000000,
            (long)gettid());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logfile, fmt, ap);
    va_end(ap);
    fputc('\n', logfile);
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
        pos += snprintf(buf + pos, sizeof(buf) - pos, "... (+%zu more)", len - show);
    }
    log_msg("  %s (%zu bytes): %s", label, len, buf);
}

static dev_type_t classify_path(const char *path) {
    if (!path) return DEV_NONE;
    if (strstr(path, "/dev/hidraw"))       return DEV_HIDRAW;
    if (strstr(path, "/dev/input/event"))  return DEV_EVDEV;
    if (strstr(path, "/dev/input/js"))     return DEV_JOYDEV;
    return DEV_NONE;
}

static const char *dev_type_str(dev_type_t t) {
    switch (t) {
        case DEV_HIDRAW: return "hidraw";
        case DEV_EVDEV:  return "evdev";
        case DEV_JOYDEV: return "joydev";
        default:         return "unknown";
    }
}

static void track_fd(int fd, const char *path, dev_type_t type) {
    if (fd < 0 || fd >= MAX_TRACKED_FDS) return;
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
            case 0x06: snprintf(buf, sizeof(buf), "EVIOCGNAME(%u)", size); return buf;
            case 0x07: snprintf(buf, sizeof(buf), "EVIOCGPHYS(%u)", size); return buf;
            case 0x08: snprintf(buf, sizeof(buf), "EVIOCGUNIQ(%u)", size); return buf;
            case 0x09: snprintf(buf, sizeof(buf), "EVIOCGPROP(%u)", size); return buf;
            case 0x90: snprintf(buf, sizeof(buf), "EVIOCGRAB(%s)", (dir & _IOC_WRITE) ? "grab" : "release"); return buf;
            default:
                /* EVIOCGBIT(ev, len) = _IOC(_IOC_READ, 'E', 0x20+ev, len) */
                if (nr >= 0x20 && nr < 0x40) {
                    snprintf(buf, sizeof(buf), "EVIOCGBIT(type=%u, len=%u)", nr - 0x20, size);
                    return buf;
                }
                /* EVIOCGABS(axis) = _IOR('E', 0x40+axis, struct input_absinfo) */
                if (nr >= 0x40 && nr < 0x80) {
                    snprintf(buf, sizeof(buf), "EVIOCGABS(axis=%u)", nr - 0x40);
                    return buf;
                }
                /* EVIOCGKEY, etc */
                snprintf(buf, sizeof(buf), "EVIOC_0x%02x(sz=%u)", nr, size);
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
            case 0x04: snprintf(buf, sizeof(buf), "HIDIOCGRAWNAME(%u)", size); return buf;
            case 0x05: snprintf(buf, sizeof(buf), "HIDIOCGRAWPHYS(%u)", size); return buf;
            case 0x06: snprintf(buf, sizeof(buf), "HIDIOCSFEATURE(%u)", size); return buf;
            case 0x07: snprintf(buf, sizeof(buf), "HIDIOCGFEATURE(%u)", size); return buf;
            case 0x08: snprintf(buf, sizeof(buf), "HIDIOCGRAWUNIQ(%u)", size); return buf;
            default:
                snprintf(buf, sizeof(buf), "HIDRAW_0x%02x(sz=%u)", nr, size);
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
        snprintf(buf, sizeof(buf), "ioctl(0x%08lx)", request);
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

    int ret = real_ioctl(fd, request, arg);

    if (t) {
        const char *name = decode_ioctl(request, t->type);
        log_msg("IOCTL fd=%d (%s %s) %s → ret=%d%s",
                fd, dev_type_str(t->type), t->path, name, ret,
                (ret < 0) ? strerror(errno) : "");

        /* Log specific ioctl results */
        if (ret == 0) {
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
            /* HIDIOCGFEATURE / HIDIOCSFEATURE */
            if (ioc_type == 'H' && (nr == 0x06 || nr == 0x07) && arg) {
                unsigned int sz = _IOC_SIZE(request);
                hex_dump("feature report", arg, sz);
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
 * NOTE: libudev hooks (udev_device_get_parent_with_subsystem_devtype, etc.)
 * are intentionally NOT included. Exporting those symbols via LD_PRELOAD
 * shadows Steam's bundled libudev and causes SDL2 to misidentify controllers.
 * The syscall-level hooks above (open, ioctl, read, write) capture all the
 * information needed for diagnosis — including HIDIOCGRAWINFO (bus type),
 * EVIOCGID (vendor/product), and HIDIOCGFEATURE (feature reports).
 */


/* ---------- constructor ---------- */

__attribute__((constructor))
static void intercept_init(void) {
    ensure_init();
    log_msg("=== ds4-intercept loaded into pid=%d (%s) ===", getpid(), program_invocation_short_name);
}

__attribute__((destructor))
static void intercept_fini(void) {
    if (logfile && logfile != stderr) {
        log_msg("=== ds4-intercept unloading ===");
        fclose(logfile);
        logfile = NULL;
    }
}
