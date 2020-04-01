#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <fcntl.h>
#include <linux/input.h>

#include <r.h>

struct options {
    const char* input_device_path;
};

struct state {
    int running;
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

static const char* input_event_type_to_string(uint16_t type)
{
    switch(type) {
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

static void read_event(int fd, struct input_event* e)
{
    debug("waiting for event");

    ssize_t r = read(fd, e, sizeof(*e));
    CHECK(r, "read");
    if(r != sizeof(*e)) { failwith("unexpected partial read"); }

    info("received event: type=%s code=%s value=%"PRId32,
          input_event_type_to_string(e->type),
          input_event_code_to_string(e->code),
          e->value);
}

static void handle_event(struct state* s, struct input_event* e)
{
    if(e->type != EV_KEY) {
        debug("ignoring event of type: %s",
              input_event_type_to_string(e->type));
        return;
    }

    if(e->code == BTN_BASE4 && e->value == 1) {
        s->running = 0;
    }
}

void init_state(struct state* s, const struct options* o)
{
    memset(s, 0, sizeof(*s));

    s->running = 1;
}

int main(int argc, char* argv[])
{
    struct options o;
    parse_options(&o, argc, argv);

    debug("input device: %s", o.input_device_path);

    struct state s;
    init_state(&s, &o);

    int fd = open(o.input_device_path, O_RDONLY);
    CHECK(fd, "open(%s)", o.input_device_path);

    struct input_event e;
    while(s.running) {
        read_event(fd, &e);
        handle_event(&s, &e);
    }

    int r = close(fd); CHECK(r, "close");

    return 0;
}
