#include <errno.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <string.h>
#include <stdlib.h>

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

    enum layout layout;

    int sfd;

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

static void x11_window_name(const struct state* st, Window w, char* buf)
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
        failwith("XGetWindowProperty(%lu, %s) failed",
                 w, XGetAtomName(st->dpy, st->wm_class));
    }

    if(t == None) {
        debug("window %lu has no name", w);
        buf[0] = 0;
        return;
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
}

static size_t x11_window_class(const struct state* st, Window w,
                               char cls[MAX_CLASS][MAX_STR])
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
        failwith("XGetWindowProperty(%lu, %s) failed",
                 w, XGetAtomName(st->dpy, st->wm_class));

    }

    if(t == None) {
        trace("window %lu has no class", w);
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

    XFree(b);
    return i;
}

static void x11_current_window(struct window* w, const struct state* st)
{
    int rt;
    if(XGetInputFocus(st->dpy, &w->window, &rt) != 1) {
        failwith("XGetInputFocus failed");
    }
    debug("focused window: %lu (%lx)", w->window, w->window);

    x11_window_name(st, w->window, w->name);
    debug("focused window name: %s", w->name);

    w->n_class = x11_window_class(st, w->window, w->class);
    for(size_t i = 0; i < w->n_class; i++) {
        debug("focused window class: %s", w->class[i]);
    }
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

static void set_layout(struct state* st, enum layout l)
{
    if(st->layout == l) {
        return;
    }

    const char* cmd;
    switch(l) {
    case DVORAK: cmd = "dv"; break;
    case ENGLISH: cmd = "us"; break;
    case SWEDISH: cmd = "sv"; break;
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
    if(window_has_class(w, "musescore")
       || window_has_class(w ,"BaldursGate")) {
        set_layout(st, ENGLISH);
    } else {
        set_layout(st, DVORAK);
    }
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
            struct window w;
            x11_current_window(&w, st);
            focus_changed(st, &w);
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

int main(int argc, char* argv[])
{
    struct state st = {
        .running = 1,

        .layout = DVORAK,
    };

    signalfd_init(&st);
    x11_init(&st);

    struct pollfd fds[] = {
        { .fd = signalfd_fd(&st), .events = POLLIN },
        { .fd = x11_fd(&st), .events = POLLIN },
    };

    while(st.running) {
        int r = poll(fds, LENGTH(fds), 0);
        CHECK(r, "poll");

        if(fds[0].revents & POLLIN) {
            signalfd_handle_event(&st);
        }

        if(fds[1].revents & POLLIN) {
            x11_handle_event(&st);
        }
    }

    debug("graceful shutdown");
    x11_deinit(&st);
    signalfd_deinit(&st);

    return 0;
}
