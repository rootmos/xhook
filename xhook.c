#include <errno.h>
#include <libudev.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#define LIBR_IMPLEMENTATION
#include "r.h"

typedef const char* layout_t;

static int handle_x11_error(Display* d, XErrorEvent* e)
{
    char buf[1024];
    XGetErrorText(d, e->error_code, LIT(buf));
    error("x11: %s", buf);
    return 0;
}

struct state {
    int running;
    Window active;

    layout_t layout;

    int sfd, tfd;

    Display* dpy;
    int scr;
    Window parent;

    Atom net_wm_name, wm_name, utf8_string, string, compound_text, wm_class;

    struct udev* udev;
    struct udev_monitor* udev_mon;
};

static void x11_init(struct state* st)
{
    XSetErrorHandler(handle_x11_error);

    st->dpy = XOpenDisplay(NULL);
    if(st->dpy == NULL) failwith("unable to open display");

    st->scr = DefaultScreen(st->dpy);
    st->parent = RootWindow(st->dpy, st->scr);

    info("tracking focus changes of %lu and its children", st->parent);
    XSelectInput(st->dpy, st->parent, FocusChangeMask);

    st->net_wm_name = XInternAtom(st->dpy, "_NET_WM_NAME", False);
    st->wm_name = XInternAtom(st->dpy, "WM_NAME", False);
    st->utf8_string = XInternAtom(st->dpy, "UTF8_STRING", False);
    st->string = XInternAtom(st->dpy, "STRING", False);
    st->compound_text = XInternAtom(st->dpy, "COMPOUND_TEXT", False);
    st->wm_class = XInternAtom(st->dpy, "WM_CLASS", False);

    XSync(st->dpy, False);
}

static void x11_deinit(struct state* st)
{
    XSync(st->dpy, True);
    XCloseDisplay(st->dpy);
}

static int x11_fd(const struct state* st)
{
    return XConnectionNumber(st->dpy);
}

#define MAX_STR 1024
#define MAX_CLASS 10

struct window
{
    Window window;
    char name[MAX_STR];
    char class[MAX_CLASS][MAX_STR];
    size_t n_class;
    Window parent, root;
};

static int x11_window_name(const struct state* st, Window w, char* buf)
{
    Atom t = None;
    int fmt;
    unsigned long nitems, remaining;
    unsigned char* b = NULL;

    Atom p = st->net_wm_name, T = st->utf8_string;

attempt:
    debug("XGetWindowProperty(%lu, %s)", w, XGetAtomName(st->dpy, p));
    int res = XGetWindowProperty(st->dpy, w, p,
                                 0L, MAX_STR-1,
                                 False /* delete */,
                                 T /* req_type */,
                                 &t /* actual_type */,
                                 &fmt, &nitems, &remaining, &b);

    if(res != Success) {
        debug("XGetWindowProperty(%lu, %s) failed",
              w, XGetAtomName(st->dpy, p));
        return -1;
    }

    if(t == None) {
        if(p == st->net_wm_name) {
            p = st->wm_name;
            T = st->string;
            goto attempt;
        }

        debug("window %lu has no name", w);
        buf[0] = 0;
        return 0;
    }

    if(t == st->compound_text) {
        warning("window %lu has COMPOUND_TEXT name: ignoring", w);
        buf[0] = 0;
        return 0;
    }

    if(t != st->utf8_string && t != st->string) {
        failwith("XGetWindowProperty(%lu, %s) returned an unexpected type: %s",
                 w, XGetAtomName(st->dpy, p),
                 XGetAtomName(st->dpy, t));
    }

    if(fmt != 8) {
        failwith("XGetWindowProperty(%lu, %s) returned an unexpected format",
                 w, XGetAtomName(st->dpy, p));
    }

    memcpy(buf, b, nitems+1);
    XFree(b);
    return 0;
}

static size_t x11_window_class(const struct state* st, Window w,
                               char cls[MAX_CLASS][MAX_STR],
                               size_t* n_cls)
{
    Atom t = None;
    int fmt;
    unsigned long nitems, remaining;
    unsigned char* b = NULL;
    int res = XGetWindowProperty(st->dpy, w, st->wm_class,
                                 0L, MAX_CLASS*MAX_STR,
                                 False /* delete */,
                                 XA_STRING /* req_type */,
                                 &t /* actual_type */,
                                 &fmt, &nitems, &remaining, &b);

    if(res != Success) {
        debug("XGetWindowProperty(%lu, %s) failed",
              w, XGetAtomName(st->dpy, st->wm_class));
        return -1;
    }

    if(t == None) {
        trace("window %lu has no class", w);
        *n_cls = 0;
        return 0;
    }

    if(t != XA_STRING) {
        failwith("XGetWindowProperty(%lu, %s) returned an unexpected type",
                 w, XGetAtomName(st->dpy, st->wm_class));
    }

    if(fmt != 8) {
        failwith("XGetWindowProperty(%lu, %s) returned an unexpected format",
                 w, XGetAtomName(st->dpy, st->wm_class));
    }

    char* p = (char*)b;
    char* P = p + nitems;
    size_t i = 0;
    while(p < P) {
        size_t l = strlen(p);
        size_t L = MIN(MAX_STR-1, l);
        memcpy(cls[i++], p, L + 1);
        p += l + 1;
    }

    *n_cls = i;

    XFree(b);
    return 0;
}

static int x11_window_parent(const struct state* st, Window w,
                             Window* root, Window* parent)
{
    Window r, p;
    Window *children = NULL;
    unsigned int children_n;
    Status res = XQueryTree(st->dpy, w,
                            root != NULL ? root : &r,
                            parent != NULL ? parent : &p,
                            &children, &children_n);

    if(!res) {
        debug("XQueryTree(%lu) failed", w);
        return -1;
    }

    XFree(children);

    return 0;
}

static int x11_window(const struct state* st, Window wx, struct window* w)
{
    w->window = wx;

    if(x11_window_name(st, wx, w->name) != 0) {
        return -1;
    }
    if(w->name[0]) {
        debug("window %lu name: %s", wx, w->name);
    }

    if(x11_window_class(st, wx, w->class, &w->n_class) != 0) {
        return -1;
    }
    for(size_t i = 0; i < w->n_class; i++) {
        debug("window %lu class: %s", wx, w->class[i]);
    }

    if(x11_window_parent(st, wx, &w->root, &w->parent) != 0) {
        return -1;
    }
    debug("window %lu root: %lu", wx, w->root);
    debug("window %lu parent: %lu", wx, w->parent);

    return 0;
}

static int window_has_class(const struct window* w, const char* cls)
{
    for(size_t i = 0; i < w->n_class; i++) {
        if(strcmp(w->class[i], cls) == 0) {
            return 1;
        }
    }

    return 0;
}

static int window_has_class_rec(const struct state* st,
                                const struct window* w, const char* cls)
{
    if(window_has_class(w, cls)) {
        return 1;
    }

    Window xw = w->parent;
    struct window p;
    do {
        if(x11_window(st, xw, &p) != 0) {
            warning("x11_window(%lu) failed", xw);
            return 0;
        }

        if(window_has_class(&p, cls)) {
            return 1;
        }

        xw = p.parent;
    } while(xw != p.root);

    return 0;
}

static int window_has_name(const struct window* w, const char* name)
{
    return strcmp(w->name, name) == 0;
}

static Window x11_current_window(const struct state* st)
{
    Window w;
    int rt;
    if(XGetInputFocus(st->dpy, &w, &rt) != 1) {
        failwith("XGetInputFocus failed");
    }
    trace("focused window: %lu (%lx)", w, w);
    return w;
}

static void set_layout(struct state* st, const layout_t l)
{
    if(st->layout == l) {
        return;
    }

    char cmd[255];
    snprintf(LIT(cmd), "~/bin/keymap %s", l);

    debug("running: %s", cmd);
    int ec = system(cmd);
    CHECK(ec, "system(%s)", cmd);

    if(ec != 0) {
        warning("changing to layout %s failed with exit code: %d", cmd, ec);
        return;
    }

    info("switched layout: %s", cmd);
    st->layout = l;
}

#include "config.h"

static void run_hooks(struct state* st, const struct window* w)
{
    debug("running hooks for window: %lu", w->window);

    layout_t l = select_layout(st, w);
    if(l) {
        set_layout(st, l);
    }
}

static void check_focus(struct state* st)
{
    Window wx = x11_current_window(st);
    if(wx == st->active) {
        return;
    }

    debug("focus changed: %lu", wx);
    st->active = wx;

    struct window w;
    if(x11_window(st, wx, &w) != 0) {
        return;
    }

    info("focus changed %lu: %s", w.window, w.name);
    run_hooks(st, &w);
}

static void udev_init(struct state* st)
{
    st->udev = udev_new();
    if(st->udev == NULL) {
        failwith("udev_new");
    }

    st->udev_mon = udev_monitor_new_from_netlink(st->udev, "udev");
    if(st->udev_mon == NULL) {
        failwith("udev_monitor_new_from_netlink");
    }

    int r = udev_monitor_filter_add_match_subsystem_devtype(
        st->udev_mon, "input", NULL);
    if(r < 0) {
        failwith("udev_monitor_filter_add_match_subsystem_devtype");
    }

    r = udev_monitor_filter_update(st->udev_mon);
    if(r < 0) {
        failwith("udev_monitor_filter_update");
    }
}

static void udev_deinit(struct state* st)
{
    udev_monitor_unref(st->udev_mon);
    udev_unref(st->udev);
}

static void udev_start(struct state* st)
{
    int r = udev_monitor_enable_receiving(st->udev_mon);
    if(r < 0) {
        failwith("udev_monitor_enable_receiving");
    }
}

static int udev_fd(const struct state* st)
{
    int fd = udev_monitor_get_fd(st->udev_mon);
    if(fd < 0) {
        failwith("udev_monitor_get_fd");
    }
    return fd;
}

static void udev_handle_event(struct state* st)
{
    struct udev_device* d = udev_monitor_receive_device(st->udev_mon);
    if(d == NULL) {
        failwith("udev_monitor_receive_device");
    }

    const char* action = udev_device_get_property_value(d, "ACTION");
    if(action == NULL) {
        debug("udev: event with action == NULL");
        return;
    } else if(strcmp(action, "add") != 0) {
        debug("udev; ignoring non-add event: %s", action);
        return;
    }

    const char* kbd = udev_device_get_property_value(d, "ID_INPUT_KEYBOARD");
    if(kbd == NULL) {
        debug("udev; ignoring non keyboard event (empty)");
        return;
    } else if(strcmp(kbd, "1") != 0) {
        debug("udev; ignoring non keyboard event (ID_INPUT_KEYBOARD=%s)", kbd);
        return;
    }

    const char* serial = udev_device_get_property_value(d, "ID_SERIAL");
    info("keyboard added: %s", serial);

    debug("resetting layout");
    st->layout = NULL;

    struct window w;
    if(x11_window(st, st->active, &w) == 0) {
        run_hooks(st, &w);
    }

    udev_device_unref(d);

    // TODO: ought one process more than one event here?
}

static void x11_handle_event(struct state* st)
{
    while(XPending(st->dpy)) {
        XEvent ev;
        int r = XNextEvent(st->dpy, &ev);
        CHECK_IF(r != Success, "XNextEvent");

        if(ev.type == FocusIn) {
            trace("focus in event: %lu", ev.xfocus.window);
        } else if(ev.type == FocusOut) {
            check_focus(st);
        } else {
            warning("ignored event: type=%d", ev.type);
        }
    }
}

static int signalfd_init(struct state* st)
{
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGINT);
    sigaddset(&m, SIGTERM);

    int fd = st->sfd = signalfd(-1, &m, 0);
    CHECK(fd, "signalfd");

    int r = sigprocmask(SIG_BLOCK, &m, NULL);
    CHECK(r, "sigprocmask");

    set_blocking(fd, 0);

    return fd;
}

static int signalfd_fd(const struct state* st)
{
    return st->sfd;
}

static void signalfd_deinit(struct state* st)
{
    int r = close(st->sfd); CHECK(r, "close");
    st->sfd = -1;
}

static void signalfd_handle_event(struct state* st)
{
    while(1) {
        struct signalfd_siginfo si;

        ssize_t s = read(st->sfd, &si, sizeof(si));
        if(s == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        CHECK(s, "read");

        if(s != sizeof(si)) {
            failwith("unexpected partial read");
        }

        if(si.ssi_signo == SIGINT) {
            debug("SIGINT");
            st->running = 0;
        } else if(si.ssi_signo == SIGTERM) {
            debug("SIGTERM");
            st->running = 0;
        } else {
            warning("unhandled signal: %u", si.ssi_signo);
        }
    }
}

static void timespec_from_ms(struct timespec* ts, unsigned int ms)
{
    unsigned int s = ts->tv_sec = ms / 1000;
    ms -= s * 1000;
    ts->tv_nsec = ms * 1000000;
}

static void timerfd_init(struct state* st)
{
    st->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    CHECK(st->tfd, "timerfd_create");
}

static void timerfd_start(struct state* st, unsigned int period_ms)
{
    struct timespec period;
    timespec_from_ms(&period, period_ms);

    struct itimerspec its = {
        .it_interval = period,
        .it_value = period,
    };
    int r = timerfd_settime(st->tfd, 0, &its, NULL);
    CHECK(r, "timerfd_settime");
}

static int timerfd_fd(const struct state* st)
{
    return st->tfd;
}

static void timerfd_deinit(struct state* st)
{
    int r = close(st->tfd); CHECK(r, "close");
    st->tfd = -1;
}

static void timerfd_ticks(struct state* st)
{
    size_t ticks = 0;
    while(1) {
        uint64_t t = 0;
        ssize_t r = read(st->tfd, &t, sizeof(t));
        if(r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        CHECK(r, "read");
        if(r != sizeof(t)) {
            failwith("unexpected partial read");
        }
        ticks += t;
    }
    if(ticks == 0) {
        failwith("spurious timerfd read");
    } else if(ticks > 1) {
        warning("missed timer ticks: %zu", ticks - 1);
    }

    trace("tick");
    check_focus(st);
}

int main(int argc, char* argv[])
{
    struct state st = {
        .running = 1,
        .active = None,

        .layout = NULL,
    };

    signalfd_init(&st);
    timerfd_init(&st);
    x11_init(&st);
    udev_init(&st);

    st.active = x11_current_window(&st);

    struct window w;
    if(x11_window(&st, st.active, &w) == 0) {
        run_hooks(&st, &w);
    }

    timerfd_start(&st, 100);
    udev_start(&st);

    struct pollfd fds[] = {
        { .fd = signalfd_fd(&st), .events = POLLIN },
        { .fd = timerfd_fd(&st), .events = POLLIN },
        { .fd = x11_fd(&st), .events = POLLIN },
        { .fd = udev_fd(&st), .events = POLLIN },
    };

    while(st.running) {
        int r = poll(fds, LENGTH(fds), -1);
        CHECK(r, "poll");

        if(fds[0].revents & POLLIN) {
            signalfd_handle_event(&st);
            fds[0].revents &= ~POLLIN;
        }

        if(fds[1].revents & POLLIN) {
            timerfd_ticks(&st);
            fds[1].revents &= ~POLLIN;
        }

        if(fds[2].revents & POLLIN) {
            x11_handle_event(&st);
            fds[2].revents &= ~POLLIN;
        }

        if(fds[3].revents & POLLIN) {
            udev_handle_event(&st);
            fds[3].revents &= ~POLLIN;
        }

        for(size_t i = 0; i < LENGTH(fds); i++) {
            if(fds[i].revents != 0) {
                failwith("unhandled poll events: "
                         "fds[%zu] = { .fd = %d, .revents = %hd }",
                         i, fds[i].fd, fds[i].revents);
            }
        }
    }

    debug("graceful shutdown");
    udev_deinit(&st);
    x11_deinit(&st);
    signalfd_deinit(&st);
    timerfd_deinit(&st);

    return 0;
}
