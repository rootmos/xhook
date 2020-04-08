#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <r.h>

Display* dpy;

struct options {
    unsigned int border_width;
    unsigned long border_color;
    int timeout_ms;
};

static int handle_x11_error(Display* d, XErrorEvent* e)
{
    char buf[1024];
    XGetErrorText(d, e->error_code, LIT(buf));
    error("x11: %s", buf);
    return 0;
}

static Window focused_window(void)
{
    Window w; int rv;

    int res = XGetInputFocus(dpy, &w, &rv);
    if(res != 1) failwith("XGetInputFocus failed: %d", res);

    trace("focused window: %lu", w);

    return w;
}

static Window create_outline(struct options* opts, Window w)
{
    XWindowAttributes wa;
    XGetWindowAttributes(dpy, w, &wa);

    int scr = DefaultScreen(dpy);
    Window parent = RootWindow(dpy, scr);

    XVisualInfo vi;
    if(!XMatchVisualInfo(dpy, scr, 32, TrueColor, &vi)) {
        failwith("no visuals found");
    }

    XSetWindowAttributes swa = {
        .override_redirect = True,
        .border_pixel = opts->border_color,
        .background_pixel = 0,
        .colormap = XCreateColormap(dpy, parent, vi.visual, AllocNone),
    };

    Window o = XCreateWindow(
        dpy,
        parent,
        wa.x, wa.y,
        wa.width-(2*opts->border_width),
        wa.height-(2*opts->border_width),
        opts->border_width,
        vi.depth,
        InputOutput,
        vi.visual,
        CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel,
        &swa);

    debug("outline window (%ld): outlining %ld", o, w);

    Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom types[] = {
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False),
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False),
    };
    XChangeProperty(dpy, o, type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char*)types, LENGTH(types));

    Atom above = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom above_value = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    XChangeProperty(dpy, o, above, XA_ATOM, 32, PropModeReplace,
                    (unsigned char*)&above_value, 1L);

    XMapRaised(dpy, o);

    return o;
}

static void update_outline(struct options* opts, Window o, Window w)
{
    XWindowAttributes wa;
    XGetWindowAttributes(dpy, w, &wa);

    debug("updating outline window (%ld): now outlining %ld", o, w);

    XMoveResizeWindow(dpy, o, wa.x, wa.y,
                      wa.width-(2*opts->border_width),
                      wa.height-(2*opts->border_width));
}

static void listen_to_window(Window w)
{
    XSelectInput(dpy, w, FocusChangeMask | StructureNotifyMask);
}

static void stop_listening_to_window(Window w)
{
    XSelectInput(dpy, w, 0);
}

static timer_t timer_init(void)
{
    timer_t t;

    struct sigevent s = {
        .sigev_notify = SIGEV_SIGNAL,
        .sigev_signo = SIGINT,
    };

    int r = timer_create(CLOCK_MONOTONIC, &s, &t);
    CHECK(r, "timer_create");

    return t;
}

static void timer_deadline(timer_t t, unsigned int ms)
{
    unsigned int s = ms / 1000;
    ms = ms - 1000*s;

    struct itimerspec ts = {
        .it_value = { .tv_sec = s, .tv_nsec = ms * 1000000 },
    };
    int r = timer_settime(t, 0, &ts, NULL);
    CHECK(r, "timer_settime");
}

static void timer_deinit(timer_t t)
{
    int r = timer_delete(t); CHECK(r, "timer_delete");
}

static int signalfd_init(void)
{
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGINT);

    int fd = signalfd(-1, &m, 0);
    CHECK(fd, "signalfd");

    int r = sigprocmask(SIG_BLOCK, &m, NULL);
    CHECK(r, "sigprocmask");

    set_blocking(fd, 0);

    return fd;
}

static void signalfd_deinit(int fd)
{
    int r = close(fd); CHECK(r, "close");
}

static void print_usage(int fd, const char* prog)
{
    dprintf(fd, "usage: %s [OPTION]...\n", prog);
    dprintf(fd, "\n");
    dprintf(fd, "options:\n");
    dprintf(fd, "  -c COLOR     draw a COLOR colored border\n");
    dprintf(fd, "  -w WIDTH     draw a border WIDTH pixels wide\n");
    dprintf(fd, "  -t MS        display the outline for MS milliseconds\n");
    dprintf(fd, "  -h           print this message\n");
}

static void parse_options(struct options* o, int argc, char* argv[])
{
    int res;
    while((res = getopt(argc, argv, "c:w:t:h")) != -1) {
        switch(res) {
        case 'c':
            res = sscanf(optarg, "%lx", &o->border_color);
            if(res != 1) {
                dprintf(2, "unable to parse border color: %s\n", optarg);
                exit(1);
            }
            break;
        case 'w':
            res = sscanf(optarg, "%u", &o->border_width);
            if(res != 1) {
                dprintf(2, "unable to parse border width: %s\n", optarg);
                exit(1);
            }
            break;
        case 't':
            res = sscanf(optarg, "%d", &o->timeout_ms);
            if(res != 1) {
                dprintf(2, "unable to parse timeout: %s\n", optarg);
                exit(1);
            }
            break;
        case 'h':
        default:
            print_usage(res == 'h' ? 1 : 2, argv[0]);
            exit(res == 'h' ? 0 : 1);
        }
    }

    debug("border width: %u pixels", o->border_width);
    debug("border color: 0x%.8lx ARGB", o->border_color);
    if(o->timeout_ms >= 0) {
        debug("timeout: %dms", o->timeout_ms);
    } else {
        debug("timeout: disabled");
    }
}

int main(int argc, char* argv[])
{
    struct options opts = {
        .border_width = 1,
        .border_color = 0xffff0000,
        .timeout_ms = 500,
    };
    parse_options(&opts, argc, argv);

    timer_t t = NULL;
    if(opts.timeout_ms >= 0) t = timer_init();

    XSetErrorHandler(handle_x11_error);

    dpy = XOpenDisplay(NULL);
    if(dpy == NULL) failwith("unable to open display");

    Window f = focused_window();
    listen_to_window(f);

    Window o = create_outline(&opts, f);

    int sfd = signalfd_init();

    XSync(dpy, False);

    if(opts.timeout_ms >= 0) timer_deadline(t, opts.timeout_ms);

    struct pollfd fds[] = {
        { .fd = sfd, .events = POLLIN },
        { .fd = XConnectionNumber(dpy), .events = POLLIN },
    };

    int running = 1;
    while(running) {
        int r = poll(fds, LENGTH(fds), 0);
        CHECK(r, "poll");

        if(fds[0].revents & POLLIN) {
            while(1) {
                // TODO: loop to read all events
                struct signalfd_siginfo si;

                ssize_t s = read(sfd, &si, sizeof(si));
                if(s == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }
                CHECK(s, "read");

                if(s != sizeof(si)) {
                    failwith("unexpected partial read");
                }

                if(si.ssi_signo == SIGINT) {
                    info("initiating graceful shutdown");
                    running = 0;
                }
            }
        }

        if(fds[1].revents & POLLIN) {
            while(XPending(dpy)) {
                XEvent ev;
                r = XNextEvent(dpy, &ev); CHECK_IF(r != Success, "XNextEvent");
                if(ev.type == FocusOut || ev.type == UnmapNotify) {
                    stop_listening_to_window(f);
                    f = focused_window();
                    listen_to_window(f);

                    update_outline(&opts, o, f);
                } else if(ev.type == ConfigureNotify) {
                    update_outline(&opts, o, f);
                } else if(ev.type == FocusIn) {
                    trace("ignoring event: FocusOut");
                } else {
                    warning("ignored event: type=%d", ev.type);
                }
            }
        }
    }

    info("shutting down");

    XUnmapWindow(dpy, o);
    XCloseDisplay(dpy);

    if(opts.timeout_ms >= 0) timer_deinit(t);
    signalfd_deinit(sfd);

    return 0;
}
