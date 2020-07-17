#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <r.h>

#ifndef SHELL
#define SHELL "/bin/sh"
#endif

static int handle_x11_error(Display* d, XErrorEvent* e)
{
    char buf[1024];
    XGetErrorText(d, e->error_code, LIT(buf));
    error("x11: %s", buf);
    return 0;
}

struct options {
    const char* input_device_path;
    const char* input_device_name;
    int input_device_name_index;
};

struct xlib_state {
    Display* dpy;
};

struct keys {
    int up, down, right, left;
    int select;
    int start;
    int a, b;
};

struct state {
    int running;
    int input_fd;
    int uinput_fd;

    struct xlib_state x;

    struct keys k;
    int mouse_mode;
    int mouse_movement_distance;
};

static void print_usage(int fd, const char* prog)
{
    dprintf(fd, "usage: %s [OPTION]...\n", prog);
    dprintf(fd, "\n");
    dprintf(fd, "options:\n");
    dprintf(fd, "  -i PATH  read input device at PATH\n");
    dprintf(fd, "  -n NAME  select input device with NAME\n");
    dprintf(fd, "  -I INDEX select the INDEX:th device with matching name\n");
    dprintf(fd, "  -h       print this message\n");
}

static void parse_options(struct options* o, int argc, char* argv[])
{
    memset(o, 0, sizeof(*o));
    o->input_device_name_index = -1;

    int res;
    while((res = getopt(argc, argv, "i:n:I:h")) != -1) {
        switch(res) {
        case 'i':
            o->input_device_path = strdup(optarg);
            CHECK_MALLOC(o->input_device_path);
            break;
        case 'n':
            o->input_device_name = strdup(optarg);
            CHECK_MALLOC(o->input_device_name);
            break;
        case 'I':
            res = sscanf(optarg, "%d", &o->input_device_name_index);
            if(res != 1) {
                dprintf(2, "unable to parse index: %s\n", optarg);
                exit(1);
            }
            break;
        case 'h':
        default:
            print_usage(res == 'h' ? 1 : 2, argv[0]);
            exit(res == 'h' ? 0 : 1);
        }
    }

    if(o->input_device_path == NULL &&
       o->input_device_name == NULL) {
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
        trace("XGetWindowProperty(%s) returned None",
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
    if(st->dpy != NULL) return;

    XSetErrorHandler(handle_x11_error);

    st->dpy = XOpenDisplay(NULL);
    if(st->dpy == NULL) failwith("unable to open display");
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
    case EV_REL: return "EV_REL";
    case EV_ABS: return "EV_ABS";
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

static int read_event(struct state* s, struct input_event* e)
{
    trace("reading event");

    ssize_t r = read(s->input_fd, e, sizeof(*e));
    if(r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    CHECK(r, "read");
    if(r != sizeof(*e)) { failwith("unexpected partial read"); }

    debug("received event: type=%s code=%s value=%"PRId32,
          input_event_type_to_string(e->type),
          input_event_code_to_string(e->code),
          e->value);

    return 1;
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

static void tiny_sleep()
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
    nanosleep(&ts, NULL);
}

struct mod {
    int shift, meta, alt, ctrl, super;
};

struct key {
    uint16_t k;
    struct mod m;
};

static void emit_key_event(struct state* s, uint16_t key, int state,
                           struct mod m)
{
    if(state) {
        if(m.shift) emit_event(s, EV_KEY, KEY_LEFTSHIFT, 1);
        if(m.meta) {
            emit_event(s, EV_KEY, KEY_LEFTMETA, 1);
            tiny_sleep();
        }
        if(m.alt) emit_event(s, EV_KEY, KEY_LEFTALT, 1);
        if(m.super) emit_event(s, EV_KEY, KEY_RIGHTMETA, 1);
        if(m.ctrl) emit_event(s, EV_KEY, KEY_LEFTCTRL, 1);
        emit_event(s, EV_SYN, SYN_REPORT, 0);
        emit_event(s, EV_KEY, key, 1);
        emit_event(s, EV_SYN, SYN_REPORT, 0);
    } else {
        emit_event(s, EV_KEY, key, 0);
        emit_event(s, EV_SYN, SYN_REPORT, 0);

        if(m.ctrl) emit_event(s, EV_KEY, KEY_LEFTCTRL, 0);
        if(m.super) emit_event(s, EV_KEY, KEY_RIGHTMETA, 0);
        if(m.alt) emit_event(s, EV_KEY, KEY_LEFTALT, 0);
        if(m.meta) emit_event(s, EV_KEY, KEY_LEFTMETA, 0);
        if(m.shift) emit_event(s, EV_KEY, KEY_LEFTSHIFT, 0);
        emit_event(s, EV_SYN, SYN_REPORT, 0);
    }
}

static void emit_key_press_mod(struct state* s, uint16_t key, struct mod m)
{
    emit_key_event(s, key, 1, m);
    emit_key_event(s, key, 0, m);
}

static void emit_key_press(struct state* s, uint16_t key)
{
    emit_key_press_mod(s, key, (struct mod){ 0 });
}

struct menu_item {
    const char* name;
    void (*callback)(struct state*, struct menu_item*);
    void* opaque;
};

static struct menu_item* run_menu(struct state* s,
                                  struct menu_item* ms, size_t n,
                                  int vertical)
{
    int i[2], o[2];
    int r = pipe(i); CHECK(r, "pipe");
    r = pipe(o); CHECK(r, "pipe");

    xlib_init(&s->x);

    char cmd[1024];
    snprintf(LIT(cmd), "dmenu -w %lu %s",
             xlib_current_window(&s->x),
             vertical ? "-l 20" : "");

    pid_t p = fork(); CHECK(p, "fork");
    if(p == 0) {
        r = close(i[1]); CHECK(r, "close");
        r = close(o[0]); CHECK(r, "close");
        r = dup2(i[0], 0); CHECK(r, "dup2");
        r = dup2(o[1], 1); CHECK(r, "dup2");
        r = close(2); CHECK(r, "close(2)");

        r = execlp(SHELL, "-" SHELL, "-c", cmd, NULL);
        CHECK(r, "execlp");
    }
    r = close(i[0]); CHECK(r, "close");
    r = close(o[1]); CHECK(r, "close");

    for(size_t k = 0; k < n; k++) {
        ssize_t s = write(i[1], ms[k].name, strlen(ms[k].name));
        CHECK(s, "write");
        if(s != strlen(ms[k].name)) {
            failwith("TODO partial write: %zd", s);
        }
        s = write(i[1], "\n", 1); CHECK(s, "write");
        if(s != 1) {
            failwith("TODO partial write: %zd", s);
        }
    }

    r = close(i[1]); CHECK(r, "close");

    char buf[1024];
    ssize_t j = 0;
    struct menu_item* choice = NULL;
    while(j < LENGTH(buf)) {
        ssize_t s = read(o[0], buf + j, LENGTH(buf) - j); CHECK(s, "read");
        if(s == 0) {
            goto done;
        }
        for(size_t k = 0; k < s; k++) {
            if(buf[j + k] == '\n') {
                buf[j + k] = 0;
                goto newline;
            }
        }
        j += s;
    }

newline:
    for(size_t k = 0; k < n; k++) {
        if(strncmp(ms[k].name, LIT(buf)) == 0) {
            choice = &ms[k];
            if(ms[k].callback) ms[k].callback(s, choice);
            goto done;
        }
    }

done:
    p = waitpid(p, NULL, 0); CHECK(p, "waitpid");
    r = close(o[0]); CHECK(r, "close");
    return choice;
}

static void goto_workspace(struct state* s, const char* ws)
{
    if(strncmp(ws, LIT("1")) == 0) {
        emit_key_press_mod(s, KEY_1, (struct mod) { .meta = 1 });
    } else if(strncmp(ws, LIT("2")) == 0) {
        emit_key_press_mod(s, KEY_2, (struct mod) { .meta = 1 });
    } else if(strncmp(ws, LIT("3")) == 0) {
        emit_key_press_mod(s, KEY_KPLEFTPAREN, (struct mod) { .meta = 1 });
    } else if(strncmp(ws, LIT("4")) == 0) {
        emit_key_press_mod(s, KEY_4, (struct mod) { .meta = 1 });
    } else if(strncmp(ws, LIT("5")) == 0) {
        emit_key_press_mod(s, KEY_LEFTBRACE, (struct mod) { .meta = 1 });
    } else if(strncmp(ws, LIT("6")) == 0) {
        emit_key_press_mod(s, KEY_EQUAL, (struct mod) { .meta = 1 });
    } else if(strncmp(ws, LIT("w")) == 0) {
        emit_key_press_mod(s, KEY_W, (struct mod) { .meta = 1 });
    } else if(strncmp(ws, LIT("v")) == 0) {
        emit_key_press_mod(s, KEY_V, (struct mod) { .meta = 1 });
    } else if(strncmp(ws, LIT("m")) == 0) {
        emit_key_press_mod(s, KEY_M, (struct mod) { .meta = 1 });
    } else if(strncmp(ws, LIT("c")) == 0) {
        emit_key_press_mod(s, KEY_C, (struct mod) { .meta = 1 });
    } else if(strncmp(ws, LIT("g")) == 0) {
        emit_key_press_mod(s, KEY_G, (struct mod) { .meta = 1 });
    } else {
        warning("goto unmapped workspace: %s", ws);
    }
}

static void send_to_workspace(struct state* s, const char* ws)
{
    if(strncmp(ws, LIT("1")) == 0) {
        emit_key_press_mod(s, KEY_1, (struct mod) { .shift = 1, .meta = 1 });
    } else if(strncmp(ws, LIT("2")) == 0) {
        emit_key_press_mod(s, KEY_2, (struct mod) { .shift = 1, .meta = 1 });
    } else if(strncmp(ws, LIT("3")) == 0) {
        emit_key_press_mod(s, KEY_KPLEFTPAREN, (struct mod) { .shift = 1, .meta = 1 });
    } else if(strncmp(ws, LIT("4")) == 0) {
        emit_key_press_mod(s, KEY_4, (struct mod) { .shift = 1, .meta = 1 });
    } else if(strncmp(ws, LIT("5")) == 0) {
        emit_key_press_mod(s, KEY_LEFTBRACE, (struct mod) { .shift = 1, .meta = 1 });
    } else if(strncmp(ws, LIT("6")) == 0) {
        emit_key_press_mod(s, KEY_EQUAL, (struct mod) { .shift = 1, .meta = 1 });
    } else if(strncmp(ws, LIT("w")) == 0) {
        emit_key_press_mod(s, KEY_W, (struct mod) { .shift = 1, .meta = 1 });
    } else if(strncmp(ws, LIT("v")) == 0) {
        emit_key_press_mod(s, KEY_V, (struct mod) { .shift = 1, .meta = 1 });
    } else if(strncmp(ws, LIT("m")) == 0) {
        emit_key_press_mod(s, KEY_M, (struct mod) { .shift = 1, .meta = 1 });
    } else if(strncmp(ws, LIT("c")) == 0) {
        emit_key_press_mod(s, KEY_C, (struct mod) { .shift = 1, .meta = 1 });
    } else if(strncmp(ws, LIT("g")) == 0) {
        emit_key_press_mod(s, KEY_G, (struct mod) { .shift = 1, .meta = 1 });
    } else {
        warning("goto unmapped workspace: %s", ws);
    }
}

static void select_workspace(struct state* s, struct menu_item* m)
{
    struct menu_item ms[] = {
        { .name = "w" },
        { .name = "v" },
        { .name = "m" },
        { .name = "c" },
        { .name = "g" },
        { .name = "1" },
        { .name = "2" },
        { .name = "3" },
        { .name = "4" },
        { .name = "5" },
        { .name = "6" },
    };

    struct menu_item* choice = run_menu(s, ms, LENGTH(ms), 0);

    ((void (*)(struct state*, const char*))m->opaque)(s, choice->name);
}

static void emit_key_press_callback(struct state* s, struct menu_item* m)
{
    struct key* k = (struct key*)m->opaque;
    emit_key_press_mod(s, k->k, k->m);
}

static void run_command_callback(struct state* s, struct menu_item* m)
{
    const char* cmd = (const char*)m->opaque;
    debug("running: %s", cmd);
    system(cmd);
}

static void launch_mpv_menu(struct state* s, struct menu_item* m)
{
    struct menu_item ms[] = {
        {
            .name = "toggle subtitles",
            .callback = emit_key_press_callback,
            .opaque = &(struct key) { .k = KEY_V },
        },{
            .name = "loop current file",
            .callback = emit_key_press_callback,
            .opaque = &(struct key) { .k = KEY_L, .m = { .shift = 1 } },
        },{
            .name = "cycle aspect ration",
            .callback = emit_key_press_callback,
            .opaque = &(struct key) { .k = KEY_A, .m = { .shift = 1 } },
        },{
            .name = "show stats",
            .callback = emit_key_press_callback,
            .opaque = &(struct key) { .k = KEY_I },
        },{
            .name = "toggle stats",
            .callback = emit_key_press_callback,
            .opaque = &(struct key) { .k = KEY_I, .m = { .shift = 1 } },
        }
    };

    run_menu(s, ms, LENGTH(ms), 1);
}

static void launch_chromium_menu(struct state* s, struct menu_item* m)
{
    struct menu_item ms[] = {
        {
            .name = "refresh",
            .callback = emit_key_press_callback,
            .opaque = &(struct key) { .k = KEY_F5 },
        },{
            .name = "spawn",
            .callback = run_command_callback,
            .opaque = "chromium",
        },{
            .name = "new tab",
            .callback = emit_key_press_callback,
            .opaque = &(struct key) { .k = KEY_T, .m = { .ctrl = 1 } },
        }
    };

    run_menu(s, ms, LENGTH(ms), 1);
}

static void launch_menu(struct state* s)
{
    pid_t p = fork(); CHECK(p, "fork");
    if(p != 0) {
        p = waitpid(p, NULL, 0); CHECK(p, "waitpid");
        return;
    }

    p = fork(); CHECK(p, "fork");
    if(p != 0) exit(0);
    s->x.dpy = NULL;

    struct menu_item ms[] = {
        {
            .name = "goto workspace",
            .callback = select_workspace,
            .opaque = goto_workspace
        },{
            .name = "send to workspace",
            .callback = select_workspace,
            .opaque = send_to_workspace
        },{
            .name = "toggle status bar",
            .callback = emit_key_press_callback,
            .opaque = &(struct key) { .k = KEY_B, .m = { .meta = 1 } },
        },{
            .name = "ESC",
            .callback = emit_key_press_callback,
            .opaque = &(struct key) { .k = KEY_ESC },
        },{
            .name = "ENTER",
            .callback = emit_key_press_callback,
            .opaque = &(struct key) { .k = KEY_ENTER },
        },{
            .name = "mpv",
            .callback = launch_mpv_menu,
        },{
            .name = "chromium",
            .callback = launch_chromium_menu,
        },{
            .name = "kill controller",
            .callback = run_command_callback,
            .opaque = "killall controller",
        }
    };

    run_menu(s, ms, LENGTH(ms), 1);
    exit(0);
}

#define DPAD_UP 0x12c
#define DPAD_RIGHT 0x12d
#define DPAD_DOWN 0x12e
#define DPAD_LEFT 0x12f

static void map_dpad_to_arrow_keys(struct state* s, struct input_event* e)
{
    s->mouse_mode = 0;

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

static void emit_mouse_movements(struct state* s)
{
    const int d = s->mouse_movement_distance / 10;
    int32_t x = (s->k.left ? -d : 0) + (s->k.right ? d : 0);
    if(x != 0) emit_event(s, EV_REL, REL_X, x);

    int32_t y = (s->k.up ? -d : 0) + (s->k.down ? d : 0);
    if(y != 0) emit_event(s, EV_REL, REL_Y, y);

    if(x != 0 || y != 0) emit_event(s, EV_SYN, SYN_REPORT, 0);
}

static void map_dpad_to_mouse(struct state* s)
{
    s->mouse_mode = 1;
    emit_mouse_movements(s);
}

static void show_window_outline(void)
{
    pid_t p = fork(); CHECK(p, "fork");
    if(p != 0) {
        p = waitpid(p, NULL, 0); CHECK(p, "waitpid");
        return;
    }

    p = fork(); CHECK(p, "fork");
    if(p != 0) exit(0);

    int r = execlp(SHELL, "-" SHELL, "-c",
                   "outline-current-window -t 200 -w 2", NULL);
    CHECK(r, "execlp");
}

static void update_key_state(struct state* s, struct input_event* e)
{
    if(e->code == DPAD_LEFT && (e->value == 1 || e->value == 0)) {
        s->k.left = e->value;
        debug("keys LEFT: %d", s->k.left);
    }

    if(e->code == DPAD_RIGHT && (e->value == 1 || e->value == 0)) {
        s->k.right = e->value;
        debug("keys RIGHT: %d", s->k.right);
    }

    if(e->code == DPAD_UP && (e->value == 1 || e->value == 0)) {
        s->k.up = e->value;
        debug("keys UP: %d", s->k.up);
    }

    if(e->code == DPAD_DOWN && (e->value == 1 || e->value == 0)) {
        s->k.down = e->value;
        debug("keys DOWN: %d", s->k.down);
    }

    if(e->code == BTN_BASE3 && (e->value == 1 || e->value == 0)) {
        s->k.select = e->value;
        debug("keys SELECT: %d", s->k.select);

        if(e->value == 1) show_window_outline();
    }

    if(e->code == BTN_BASE4 && (e->value == 1 || e->value == 0)) {
        s->k.start = e->value;
        debug("keys START: %d", s->k.start);
    }

    if(e->code == BTN_THUMB && (e->value == 1 || e->value == 0)) {
        s->k.a = e->value;
        debug("keys A: %d", s->k.a);
    }

    if(e->code == BTN_THUMB2 && (e->value == 1 || e->value == 0)) {
        s->k.b = e->value;
        debug("key B: %d", s->k.b);
    }
}

static void handle_timeout(struct state* s)
{
    if(s->mouse_mode) {
        if(!s->k.up && !s->k.down && !s->k.left && !s->k.right) {
            s->mouse_mode = 0;
            s->mouse_movement_distance = 10;
        } else {
            emit_mouse_movements(s);
            s->mouse_movement_distance += 1;
        }
    }
}

static void handle_event(struct state* s, struct input_event* e)
{
    if(e->type != EV_KEY) {
        trace("ignoring event of type: %s",
              input_event_type_to_string(e->type));
        return;
    }

    update_key_state(s, e);

    Window w = xlib_current_window(&s->x);

    if(s->k.select) {
        if(e->code == DPAD_UP && e->value == 1) {
            emit_key_press_mod(s, KEY_TAB, (struct mod) { .alt = 1 });

            show_window_outline();
        }

        if(e->code == DPAD_DOWN && e->value == 1) {
            emit_key_press_mod(s, KEY_ENTER, (struct mod) { .alt = 1 });
        }

        if(e->code == DPAD_LEFT && e->value == 1) {
            emit_key_press_mod(s, KEY_H, (struct mod) { .alt = 1 });

            show_window_outline();
        }

        if(e->code == DPAD_RIGHT && e->value == 1) {
            emit_key_press_mod(s, KEY_L, (struct mod) { .alt = 1 });

            show_window_outline();
        }

        if(e->code == BTN_BASE4 && e->value == 1) {
            launch_menu(s);
        }

        if(e->code == BTN_THUMB && e->value == 1) {
            emit_key_press_mod(s, KEY_SPACE, (struct mod) { .alt = 1 });
        }

        if(e->code == BTN_THUMB2 && e->value == 1) {
            emit_key_press_mod(s, KEY_C,
                               (struct mod) { .shift = 1, .alt = 1 });
        }

        return;
    }

    if(e->code == BTN_BASE4 && e->value == 1) {
        emit_key_press_mod(s, KEY_K, (struct mod) { .alt = 1 });
    }

    if(xlib_window_has_class(&s->x, w, "feh")) {
        if(s->k.b) {
            if(e->code == DPAD_UP && (e->value == 1 || e->value == 0)) {
                emit_key_event(s, KEY_UP, e->value,
                               (struct mod) { .ctrl = 1});
            }

            if(e->code == DPAD_DOWN && (e->value == 1 || e->value == 0)) {
                emit_key_event(s, KEY_DOWN, e->value,
                               (struct mod) { .ctrl = 1});
            }

            if(e->code == DPAD_LEFT && (e->value == 1 || e->value == 0)) {
                emit_key_event(s, KEY_LEFT, e->value,
                               (struct mod) { .ctrl = 1});
            }

            if(e->code == DPAD_RIGHT && (e->value == 1 || e->value == 0)) {
                emit_key_event(s, KEY_RIGHT, e->value,
                               (struct mod) { .ctrl = 1});
            }

            if(e->code == BTN_THUMB && e->value == 1) {
                emit_key_press(s, KEY_Z);
            }
        } else {
            map_dpad_to_arrow_keys(s, e);

            if(e->code == BTN_THUMB && e->value == 1) {
                emit_key_press(s, KEY_H);
            }
        }
    } else if(xlib_window_has_class(&s->x, w, "mpv")) {
        if(s->k.b) {
            if(e->code == BTN_THUMB && e->value == 1) {
                emit_key_press(s, KEY_M);
            }

            if(e->code == DPAD_UP && e->value == 1) {
                emit_key_press(s, KEY_L);
            }

            if(e->code == DPAD_DOWN && e->value == 1) {
                emit_key_press_mod(s, KEY_L, (struct mod) { .shift = 1 });
            }

            if(e->code == DPAD_RIGHT && e->value == 1) {
                emit_key_press(s, KEY_ENTER);
            }

            if(e->code == DPAD_LEFT && e->value == 1) {
                emit_key_press(s, KEY_102ND);
            }
        } else {
            if(e->code == BTN_THUMB && e->value == 1) {
                emit_key_press(s, KEY_SPACE);
            }

            map_dpad_to_arrow_keys(s, e);
        }
    } else if(xlib_window_has_class(&s->x, w, "streamlink-twitch-gui")) {
        if(s->k.b) {
            if(e->code == BTN_THUMB && e->value == 1) {
                emit_key_press(s, KEY_F5);
            }
        } else {
            if(e->code == BTN_THUMB && (e->value == 1 || e->value == 0)) {
                emit_event(s, EV_KEY, BTN_LEFT, e->value);
                emit_event(s, EV_SYN, SYN_REPORT, 0);
            }

            map_dpad_to_mouse(s);
        }
    } else if(xlib_window_has_class(&s->x, w, "chromium")) {
        if(s->k.b) {
            if(e->code == DPAD_UP && e->value == 1) {
                emit_key_press(s, KEY_F);
            }

            if(e->code == DPAD_RIGHT && (e->value == 1 || e->value == 0)) {
                emit_event(s, EV_KEY, KEY_RIGHT, e->value);
                emit_event(s, EV_SYN, SYN_REPORT, 0);
            }

            if(e->code == DPAD_LEFT && (e->value == 1 || e->value == 0)) {
                emit_event(s, EV_KEY, KEY_LEFT, e->value);
                emit_event(s, EV_SYN, SYN_REPORT, 0);
            }

            if(e->code == DPAD_DOWN && e->value == 1) {
                emit_key_press(s, KEY_F5);
            }

            if(e->code == BTN_THUMB && e->value == 1) {
                emit_key_press(s, KEY_SPACE);
            }

            s->mouse_mode = 0;
        } else {
            if(e->code == BTN_THUMB && (e->value == 1 || e->value == 0)) {
                emit_event(s, EV_KEY, BTN_LEFT, e->value);
                emit_event(s, EV_SYN, SYN_REPORT, 0);
            }

            map_dpad_to_mouse(s);
        }
    } else if(xlib_window_has_class(&s->x, w, "dmenu")) {
        if(e->code == BTN_THUMB && e->value == 1) {
            emit_key_press(s, KEY_ENTER);
        }

        if(e->code == BTN_THUMB2 && e->value == 1) {
            emit_key_press(s, KEY_ESC);
        }

        map_dpad_to_arrow_keys(s, e);
    } else if(xlib_window_has_class(&s->x, w, "spotify")) {
        if(s->k.b) {
            if(e->code == BTN_THUMB && e->value == 1) {
                emit_key_press(s, KEY_SPACE);
            }

            if(e->code == DPAD_UP && (e->value == 1 || e->value == 0)) {
                emit_event(s, EV_KEY, KEY_UP, e->value);
                emit_event(s, EV_SYN, SYN_REPORT, 0);
            }

            if(e->code == DPAD_DOWN && (e->value == 1 || e->value == 0)) {
                emit_event(s, EV_KEY, KEY_DOWN, e->value);
                emit_event(s, EV_SYN, SYN_REPORT, 0);
            }

            if(e->code == DPAD_RIGHT && e->value == 1) {
                emit_key_press(s, KEY_ENTER);
            }

            s->mouse_mode = 0;
        } else {
            if(e->code == BTN_THUMB && e->value == 1) {
                emit_key_press(s, BTN_LEFT);
            }

            map_dpad_to_mouse(s);
        }
    } else if(xlib_window_has_class(&s->x, w, "obs")) {
        if(s->k.b) {
            if(e->code == DPAD_UP && e->value == 1) {
                emit_key_press(s, KEY_F9);
            }

            if(e->code == DPAD_DOWN && e->value == 1) {
                emit_key_press_mod(s, KEY_F9, (struct mod) { .shift = 1 });
            }

            if(e->code == BTN_THUMB && e->value == 1) {
                emit_key_press(s, KEY_F10);
            }
        } else {
            if(e->code == DPAD_DOWN && e->value == 1) {
                emit_key_press(s, KEY_F3);
            }

            if(e->code == DPAD_UP && e->value == 1) {
                emit_key_press(s, KEY_F5);
            }

            if(e->code == DPAD_LEFT && e->value == 1) {
                emit_key_press(s, KEY_F1);
            }

            if(e->code == DPAD_RIGHT && e->value == 1) {
                emit_key_press(s, KEY_F2);
            }

            if(e->code == BTN_THUMB && e->value == 1) {
                emit_key_press(s, KEY_F8);
            }
        }
    } else if(xlib_window_has_class(&s->x, w, "Sausage.x86_64")) {
        if(e->code == BTN_THUMB && e->value == 1) {
            emit_key_press(s, KEY_Z);
        }

        if(e->code == BTN_THUMB2 && e->value == 1) {
            emit_key_press(s, KEY_R);
        }

        map_dpad_to_arrow_keys(s, e);
    }
}

static int find_device_based_on_name(const char* name, int index)
{
    int choice = -1;

    int dfd = open("/dev/input", O_RDONLY); CHECK(dfd, "open(/dev/input)");
    DIR* d = fdopendir(dfd); CHECK_NOT(d, NULL, "opendir(/dev/input)");
    struct dirent* p;
    while((p = readdir(d)) != NULL) {
        if(strncmp(p->d_name, "event", 5) == 0) {
            int fd = openat(dfd, p->d_name, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
            if(fd == -1 && errno == EACCES) {
                continue;
            }
            CHECK(fd, "open(%s, O_RDONLY)", p->d_name);

            char n[256];
            int r = ioctl(fd, EVIOCGNAME(sizeof(n)), n);
            CHECK(r, "ioctl(EVIOCGNAME)");

            if(strcmp(name, n) == 0) {
                if(index >= 0) {
                    char l[256];
                    r = ioctl(fd, EVIOCGPHYS(sizeof(l)), l);
                    CHECK(r, "ioctl(EVIOCGPHYS)");
                    char buf[256]; int i = -1;
                    r = sscanf(l, "%255[^/]/input%d", buf, &i);
                    CHECK(r, "sscanf");

                    if(index == i) {
                        debug("chose input device: /dev/input/%s", p->d_name);
                        choice = fd;
                    } else {
                        r = close(fd); CHECK(r, "close");
                    }
                } else {
                    if(choice >= 0) {
                        failwith("found more than one device with matching "
                                 "name: desired device index not specified");
                    }
                    choice = fd;
                }
            } else {
                r = close(fd); CHECK(r, "close");
            }
        }
    }

    int r = closedir(d); CHECK(r, "closedir");

    return choice;
}

static void state_init(struct state* s, const struct options* o)
{
    memset(s, 0, sizeof(*s));

    s->running = 1;

    if(o->input_device_path != NULL) {
        info("input device: %s", o->input_device_path);
        s->input_fd = open(o->input_device_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        CHECK(s->input_fd, "open(%s)", o->input_device_path);
    } else if(o->input_device_name != NULL) {
        s->input_fd = find_device_based_on_name(
            o->input_device_name, o->input_device_name_index);
        if(s->input_fd == -1) {
            failwith("unable to find device with name: %s",
                     o->input_device_name);
        }
    } else {
        failwith("input device not specified");
    }

    const char* uinput_path = "/dev/uinput";
    s->uinput_fd = open(uinput_path, O_WRONLY | O_CLOEXEC);
    CHECK(s->uinput_fd, "open(%s)", uinput_path);

    int r;
    r = ioctl(s->uinput_fd, UI_SET_EVBIT, EV_KEY); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_EVBIT, EV_SYN); CHECK(r, "ioctl");

    r = ioctl(s->uinput_fd, UI_SET_EVBIT, EV_REL); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_RELBIT, REL_X); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_RELBIT, REL_Y); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, BTN_LEFT); CHECK(r, "ioctl");

    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_F1); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_F2); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_F3); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_F4); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_F5); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_F6); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_F7); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_F8); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_F9); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_F10); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_F11); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_F12); CHECK(r, "ioctl");

    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_1); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_2); CHECK(r, "ioctl");

    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_B); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_C); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_F); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_G); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_H); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_I); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_K); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_L); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_M); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_Q); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_R); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_S); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_T); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_V); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_W); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_Z); CHECK(r, "ioctl");

    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_SLASH); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_KPASTERISK); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_KPLEFTPAREN); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_DOLLAR); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_LEFTBRACE); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_EQUAL); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_102ND); CHECK(r, "ioctl");

    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_UP); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_DOWN); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_LEFT); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_RIGHT); CHECK(r, "ioctl");

    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_ESC); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_ENTER); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_SPACE); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_TAB); CHECK(r, "ioctl");

    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_LEFTALT); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_RIGHTALT); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_LEFTSHIFT); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_LEFTCTRL); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_LEFTMETA); CHECK(r, "ioctl");
    r = ioctl(s->uinput_fd, UI_SET_KEYBIT, KEY_RIGHTMETA); CHECK(r, "ioctl");

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

    struct state s;
    state_init(&s, &o);

    struct input_event e;
    while(s.running) {
        struct pollfd fds[] = {
            { .fd = s.input_fd, .events = POLLIN },
        };

        int timeout = s.mouse_mode ? 10 : 10000;
        int r = poll(fds, LENGTH(fds), timeout); CHECK(r, "poll");
        if(r == 0) {
            debug("poll timeout: mouse_mode=%d timeout=%dms",
                  s.mouse_mode, timeout);
            handle_timeout(&s);
        } else {
            trace("poll events: %d", r);

            if(fds[0].revents & POLLERR) {
                if(fds[0].revents & POLLHUP) {
                    warning("input device disconnected");
                    s.running = 0;
                    fds[0].revents &= ~POLLHUP;
                } else {
                    failwith("unhandled poll error condition: %hd",
                             fds[0].revents);
                }

                fds[0].revents &= ~POLLERR;
            }

            if(fds[0].revents & POLLIN) {
                while(read_event(&s, &e)) {
                    handle_event(&s, &e);
                }

                fds[0].revents &= ~POLLIN;
            }

            for(size_t i = 0; i < LENGTH(fds); i++) {
                if(fds[i].revents != 0) {
                    failwith("unhandled events: "
                             "fds[%zu] = { .fd = %d, .revents = %hd }",
                             i, fds[i].fd, fds[i].revents);
                }
            }
        }
    }

    state_deinit(&s);

    return 0;
}
