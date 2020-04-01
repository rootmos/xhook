#include <fcntl.h>
#include <inttypes.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

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

struct options {
    const char* input_device_path;
};

struct xlib_state {
    Display* dpy;
};

struct state {
    int running;
    int input_fd;
    int uinput_fd;

    struct xlib_state x;
};

static void print_usage(int fd, const char* prog)
{
    dprintf(fd, "usage: %s [OPTION]...\n", prog);
    dprintf(fd, "\n");
    dprintf(fd, "options:\n");
    dprintf(fd, "  -i PATH  read input device at PATH\n");
    dprintf(fd, "  -h       print this message\n");
}

static void parse_options(struct options* o, int argc, char* argv[])
{
    memset(o, 0, sizeof(*o));

    int res;
    while((res = getopt(argc, argv, "i:h")) != -1) {
        switch(res) {
        case 'i':
            o->input_device_path = strdup(optarg);
            CHECK_MALLOC(o->input_device_path);
            break;
        case 'h':
        default:
            print_usage(res == 'h' ? 1 : 2, argv[0]);
            exit(res == 'h' ? 0 : 1);
        }
    }

    if(o->input_device_path == NULL) {
        dprintf(2, "input device not specified\n");
        print_usage(2, argv[0]);
        exit(1);
    }
}

static Window xlib_current_window(struct xlib_state* st)
{
    Window w; int rv;

    int res = XGetInputFocus(st->dpy, &w, &rv);
    if(res != 1) failwith("XGetInputFocus failed: %d", res);

    trace("focused window: %lu", w);

    return w;
}

const char* xlib_window_name(struct xlib_state* st, Window w)
{
    static char buf[1024];

    Atom a = XInternAtom(st->dpy, "_NET_WM_NAME", False);
    Atom T = XInternAtom(st->dpy, "UTF8_STRING", False);

    Atom t = None;
    int fmt;
    unsigned long nitems, remaining;
    unsigned char* b = NULL;
    int res = XGetWindowProperty(st->dpy, w, a, 0L, sizeof(buf), False, T,
                                 &t, &fmt, &nitems, &remaining, &b);

    if(res != Success) {
        failwith("XGetWindowProperty failed");
    }

    if(t == None) {
        failwith("XGetWindowProperty(%s) returned None",
                 XGetAtomName(st->dpy, a));
    }

    if(t != T) {
        failwith("XGetWindowProperty(%s) returned an unexpected type",
                 XGetAtomName(st->dpy, a));
    }

    if(fmt != 8) {
        failwith("XGetWindowProperty(%s) returned an unexpected format",
                 XGetAtomName(st->dpy, a));
    }

    memcpy(buf, b, nitems+1);
    XFree(b);
    return buf;
}

static int xlib_window_has_class(struct xlib_state* st,
                                 Window w, const char* cls)
{
    Atom a = XInternAtom(st->dpy, "WM_CLASS", False);

    Atom t = None;
    int fmt;
    unsigned long nitems, remaining;
    unsigned char* b = NULL;
    int res = XGetWindowProperty(st->dpy, w, a, 0L, 1024L, False, XA_STRING,
                                 &t, &fmt, &nitems, &remaining, &b);

    if(res != Success) {
        failwith("XGetWindowProperty failed");
    }

    if(t == None) {
        warning("XGetWindowProperty(%s) returned None",
                XGetAtomName(st->dpy, a));
        return 0;
    }

    if(t != XA_STRING) {
        failwith("XGetWindowProperty(%s) returned an unexpected type",
                 XGetAtomName(st->dpy, a));
    }

    if(fmt != 8) {
        failwith("XGetWindowProperty(%s) returned an unexpected format",
                 XGetAtomName(st->dpy, a));
    }

    char* i = (char*)b, *I = i + nitems;
    while(i < I) {
        if(strcmp(i, cls) == 0) return 1;
        i += strlen(i) + 1;
    }

    XFree(b);
    return 0;
}

static void xlib_init(struct xlib_state* st)
{
    XSetErrorHandler(handle_x11_error);

    st->dpy = XOpenDisplay(NULL);
    if(st->dpy == NULL) failwith("unable to open display");

    xlib_current_window(st);
}

static void xlib_deinit(struct xlib_state* st)
{
    XCloseDisplay(st->dpy);
}


static const char* input_event_type_to_string(uint16_t type)
{
    switch(type) {
    case EV_SYN: return "EV_SYN";
    case EV_KEY: return "EV_KEY";
    case EV_MSC: return "EV_MSC";
    }

    static char buf[12];
    snprintf(LIT(buf), "0x%"PRIx16, type);
    return buf;
}

static const char* input_event_code_to_string(uint16_t code)
{
    switch(code) {
    case BTN_THUMB: return "BTN_THUMB";
    case BTN_THUMB2: return "BTN_THUMB2";
    case BTN_BASE3: return "BTN_BASE3";
    case BTN_BASE4: return "BTN_BASE4";
    }

    static char buf[12];
    snprintf(LIT(buf), "0x%"PRIx16, code);
    return buf;
}

static void read_event(struct state* s, struct input_event* e)
{
    trace("waiting for event");

    ssize_t r = read(s->input_fd, e, sizeof(*e));
    CHECK(r, "read");
    if(r != sizeof(*e)) { failwith("unexpected partial read"); }

    debug("received event: type=%s code=%s value=%"PRId32,
          input_event_type_to_string(e->type),
          input_event_code_to_string(e->code),
          e->value);
}

static void emit_event(struct state* s,
                       uint16_t type, uint16_t code, int32_t value)
{
    struct input_event e = {
        .type = type,
        .code = code,
        .value = value
    };

    ssize_t w = write(s->uinput_fd, &e, sizeof(e));
    CHECK(w, "write");

    if(w != sizeof(e)) {
        failwith("unexpected partial write");
    }

    debug("sent event: type=%s code=%s value=%"PRId32,
          input_event_type_to_string(e.type),
          input_event_code_to_string(e.code),
          e.value);
}

static void emit_key_press(struct state* s, uint16_t key)
{
    emit_event(s, EV_KEY, key, 1);
    emit_event(s, EV_SYN, SYN_REPORT, 0);

    emit_event(s, EV_KEY, key, 0);
    emit_event(s, EV_SYN, SYN_REPORT, 0);
}

#define DPAD_UP 0x12c
#define DPAD_RIGHT 0x12d
#define DPAD_DOWN 0x12e
#define DPAD_LEFT 0x12f

static void map_dpad_to_arrow_keys(struct state* s, struct input_event* e)
{
    if(e->code == DPAD_UP && (e->value == 1 || e->value == 0)) {
        emit_event(s, EV_KEY, KEY_UP, e->value);
        emit_event(s, EV_SYN, SYN_REPORT, 0);
    }

    if(e->code == DPAD_DOWN && (e->value == 1 || e->value == 0)) {
        emit_event(s, EV_KEY, KEY_DOWN, e->value);
        emit_event(s, EV_SYN, SYN_REPORT, 0);
    }

    if(e->code == DPAD_RIGHT && (e->value == 1 || e->value == 0)) {
        emit_event(s, EV_KEY, KEY_RIGHT, e->value);
        emit_event(s, EV_SYN, SYN_REPORT, 0);
    }

    if(e->code == DPAD_LEFT && (e->value == 1 || e->value == 0)) {
        emit_event(s, EV_KEY, KEY_LEFT, e->value);
        emit_event(s, EV_SYN, SYN_REPORT, 0);
    }
}

static void handle_event(struct state* s, struct input_event* e)
{
    if(e->type != EV_KEY) {
        trace("ignoring event of type: %s",
              input_event_type_to_string(e->type));
        return;
    }

    Window w = xlib_current_window(&s->x);

    if(xlib_window_has_class(&s->x, w, "feh")) {
        map_dpad_to_arrow_keys(s, e);
    } else if(xlib_window_has_class(&s->x, w, "mpv")) {
        map_dpad_to_arrow_keys(s, e);

        if(e->code == BTN_THUMB && e->value == 1) {
            emit_key_press(s, KEY_SPACE);
        }

        if(e->code == BTN_THUMB2 && e->value == 1) {
            emit_key_press(s, KEY_Q);
        }
    } else if(xlib_window_has_class(&s->x, w, "dmenu")) {
        map_dpad_to_arrow_keys(s, e);

        if(e->code == BTN_THUMB && e->value == 1) {
            emit_key_press(s, KEY_ENTER);
        }

        if(e->code == BTN_THUMB2 && e->value == 1) {
            emit_key_press(s, KEY_ESC);
        }
    } else {
        map_dpad_to_arrow_keys(s, e);

        if(e->code == BTN_THUMB2 && e->value == 1) {
            debug("initiating graceful shutdown");
            s->running = 0;
        }
    }

    if(e->code == BTN_BASE4 && e->value == 1) {
        pid_t p = fork(); CHECK(p, "fork");
        if(p == 0) {
            p = fork(); CHECK(p, "fork");
            if(p == 0) {
                int r = execlp("k", "k", "-m", "-d", getenv("HOME"), NULL);
                CHECK(r, "execlp");
            }
            exit(0);
        }
        pid_t q = waitpid(p, NULL, 0); CHECK(q, "waitpid");
    }
}

static void state_init(struct state* s, const struct options* o)
{
    memset(s, 0, sizeof(*s));

    s->running = 1;

    s->input_fd = open(o->input_device_path, O_RDONLY);
    CHECK(s->input_fd, "open(%s)", o->input_device_path);

    const char* uinput_path = "/dev/uinput";
    s->uinput_fd = open(uinput_path, O_WRONLY);
    CHECK(s->uinput_fd, "open(%s)", uinput_path);

    int r = ioctl(s->uinput_fd, UI_SET_EVBIT, EV_KEY); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_ESC); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_ENTER); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_SPACE); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_Q); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_UP); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_DOWN); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_LEFT); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_RIGHT); CHECK(r, "ioctl");

    struct uinput_setup us = {
        .id.bustype = BUS_USB,
        .id.vendor = 0x1234, /* sample vendor */
        .id.product = 0x5678, /* sample product */
        .name = "controller uinput device",
    };

    r = ioctl(s->uinput_fd, UI_DEV_SETUP, &us); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_DEV_CREATE); CHECK(r, "ioctl");

    xlib_init(&s->x);
}

static void state_deinit(struct state* s)
{
    xlib_deinit(&s->x);

    int r = close(s->input_fd); CHECK(r, "close input device");

    r = ioctl(s->uinput_fd, UI_DEV_DESTROY); CHECK(r, "ioctl");
    r = close(s->uinput_fd); CHECK(r, "close uinput");
}

int main(int argc, char* argv[])
{
    struct options o;
    parse_options(&o, argc, argv);

    debug("input device: %s", o.input_device_path);

    struct state s;
    state_init(&s, &o);

    struct input_event e;
    while(s.running) {
        read_event(&s, &e);
        handle_event(&s, &e);
    }

    state_deinit(&s);

    return 0;
}
