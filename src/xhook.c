#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <r.h>

static int handle_x11_error(Display* d, XErrorEvent* e)
{
    char buf[1024];
    XGetErrorText(d, e->error_code, LIT(buf));
    error("x11: %s", buf);
    return 0;
}

enum layout {
    DVORAK,
    ENGLISH,
    SWEDISH,
};

struct state {
    int running;
    Window active;

    enum layout layout;

    int sfd, tfd;

    Display* dpy;
    int scr;
    Window parent;

    Atom net_wm_name, utf8_string, wm_class;
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
    st->utf8_string = XInternAtom(st->dpy, "UTF8_STRING", False);
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
};

static int x11_window_name(const struct state* st, Window w, char* buf)
{
    Atom t = None;
    int fmt;
    unsigned long nitems, remaining;
    unsigned char* b = NULL;
    int res = XGetWindowProperty(st->dpy, w, st->net_wm_name,
                                 0L, MAX_STR-1,
                                 False /* delete */,
                                 st->utf8_string /* req_type */,
                                 &t /* actual_type */,
                                 &fmt, &nitems, &remaining, &b);

    if(res != Success) {
        debug("XGetWindowProperty(%lu, %s) failed",
              w, XGetAtomName(st->dpy, st->wm_class));
        return -1;
    }

    if(t == None) {
        debug("window %lu has no name", w);
        buf[0] = 0;
        return 0;
    }

    if(t != st->utf8_string) {
        failwith("XGetWindowProperty(%lu, %s) returned an unexpected type: %s",
                 w, XGetAtomName(st->dpy, st->net_wm_name),
                 XGetAtomName(st->dpy, t));
    }

    if(fmt != 8) {
        failwith("XGetWindowProperty(%lu, %s) returned an unexpected format",
                 w, XGetAtomName(st->dpy, st->net_wm_name));
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

static int x11_window(const struct state* st, Window wx, struct window* w)
{
    w->window = wx;

    if(x11_window_name(st, wx, w->name) != 0) {
        return -1;
    }
    debug("window %lu name: %s", wx, w->name);

    if(x11_window_class(st, wx, w->class, &w->n_class) != 0) {
        return -1;
    }
    for(size_t i = 0; i < w->n_class; i++) {
        debug("window %lu class: %s", wx, w->class[i]);
    }

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

static void set_layout(struct state* st, enum layout l)
{
    if(st->layout == l) {
        return;
    }

    const char* cmd;
    switch(l) {
    case DVORAK: cmd = "~/bin/dv"; break;
    case ENGLISH: cmd = "~/bin/us"; break;
    case SWEDISH: cmd = "~/bin/sv"; break;
    default: failwith("unsupported layout: %d", l);
    }

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

static void focus_changed(struct state* st, const struct window* w)
{
    info("focus changed %lu: %s", w->window, w->name);

    if(window_has_class(w, "musescore")
       || window_has_class(w, "BaldursGate")
       || window_has_class(w, "Dwarf_Fortress")
       || window_has_class(w, "nethack")
       || window_has_name(w, "Caesar III")
       || window_has_class(w, "devilutionx")
       ) {
        set_layout(st, ENGLISH);
    } else {
        set_layout(st, DVORAK);
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

    focus_changed(st, &w);
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

        .layout = DVORAK,
    };

    signalfd_init(&st);
    timerfd_init(&st);
    x11_init(&st);

    st.active = x11_current_window(&st);

    timerfd_start(&st, 100);

    struct pollfd fds[] = {
        { .fd = signalfd_fd(&st), .events = POLLIN },
        { .fd = timerfd_fd(&st), .events = POLLIN },
        { .fd = x11_fd(&st), .events = POLLIN },
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

        for(size_t i = 0; i < LENGTH(fds); i++) {
            if(fds[i].revents != 0) {
                failwith("unhandled poll events: "
                         "fds[%zu] = { .fd = %d, .revents = %hd }",
                         i, fds[i].fd, fds[i].revents);
            }
        }
    }

    debug("graceful shutdown");
    x11_deinit(&st);
    signalfd_deinit(&st);
    timerfd_deinit(&st);

    return 0;
}
