/*
 * joystick_bridge.c
 *
 * Translates gamepad/joystick Linux input events into keyboard events via
 * uinput, so that terminal programs (e.g. `dialog`) can be navigated with
 * a gamepad even after the SDL parent process has exited.
 *
 * Usage:
 *   joystick_bridge <joystick-event-device> <child-pid>
 *
 * Example (from ConsoleMode before launching dialog):
 *   dialog --menu ... &
 *   DIALOG_PID=$!
 *   joystick_bridge /dev/input/event2 $DIALOG_PID &
 *   wait $DIALOG_PID
 *
 * Or from C (see README.md for the ConsoleMode integration snippet):
 *   pid_t dialog_pid = fork_and_exec_dialog(...);
 *   fork_and_exec_bridge(joystick_dev, dialog_pid);
 *   // SDL program can now exit safely
 *
 * Default gamepad → key mapping (MiSTer standard layout):
 *   D-pad up        → KEY_UP
 *   D-pad down      → KEY_DOWN
 *   D-pad left      → KEY_LEFT
 *   D-pad right     → KEY_RIGHT
 *   Button South(A) → KEY_ENTER
 *   Button East (B) → KEY_ESC
 *   Button West (X) → KEY_SPACE
 *   Button North(Y) → KEY_TAB
 *   Left shoulder   → KEY_PAGEUP
 *   Right shoulder  → KEY_PAGEDOWN
 *   Select/Back     → KEY_ESC
 *   Start           → KEY_ENTER
 *
 * The bridge monitors the child PID and exits automatically when the child
 * process terminates.
 *
 * Build: see Makefile
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/input.h>
#include <linux/uinput.h>

/* -----------------------------------------------------------------------
 * Key mapping tables
 * ----------------------------------------------------------------------- */

/* EV_KEY (button) → keyboard key */
struct btn_map {
    uint16_t btn_code;   /* Linux BTN_* code from gamepad */
    uint16_t key_code;   /* KEY_* to emit via uinput */
};

static const struct btn_map BTN_MAP[] = {
    { BTN_SOUTH,   KEY_ENTER },   /* A / Cross      */
    { BTN_EAST,    KEY_ESC   },   /* B / Circle     */
    { BTN_WEST,    KEY_SPACE },   /* X / Square     */
    { BTN_NORTH,   KEY_TAB   },   /* Y / Triangle   */
    { BTN_TL,      KEY_PAGEUP },  /* L1             */
    { BTN_TR,      KEY_PAGEDOWN },/* R1             */
    { BTN_SELECT,  KEY_ESC   },   /* Select / Back  */
    { BTN_START,   KEY_ENTER },   /* Start          */
    { BTN_DPAD_UP,    KEY_UP    },
    { BTN_DPAD_DOWN,  KEY_DOWN  },
    { BTN_DPAD_LEFT,  KEY_LEFT  },
    { BTN_DPAD_RIGHT, KEY_RIGHT },
};
static const size_t BTN_MAP_LEN = sizeof(BTN_MAP) / sizeof(BTN_MAP[0]);

/*
 * D-pad delivered as EV_ABS ABS_HAT0X / ABS_HAT0Y (most USB gamepads and
 * MiSTer wireless receivers).
 */
struct hat_state {
    int x;   /* ABS_HAT0X: -1=left, 0=centre, +1=right */
    int y;   /* ABS_HAT0Y: -1=up,   0=centre, +1=down  */
};

/* -----------------------------------------------------------------------
 * uinput helpers
 * ----------------------------------------------------------------------- */

static int uinput_fd = -1;

static int uinput_emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    return (int)write(fd, &ev, sizeof(ev));
}

static int uinput_key(int fd, uint16_t key_code, int pressed)
{
    if (uinput_emit(fd, EV_KEY, key_code, pressed ? 1 : 0) < 0)
        return -1;
    /* sync event */
    return uinput_emit(fd, EV_SYN, SYN_REPORT, 0);
}

static int uinput_open(void)
{
    int fd;
    struct uinput_setup usetup;

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        /* Fallback path used on some kernels */
        fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    }
    if (fd < 0) {
        perror("joystick_bridge: open /dev/uinput");
        return -1;
    }

    /* Enable EV_KEY */
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) {
        perror("joystick_bridge: UI_SET_EVBIT");
        close(fd);
        return -1;
    }

    /* Register every key we may emit */
    static const uint16_t keys[] = {
        KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
        KEY_ENTER, KEY_ESC, KEY_SPACE, KEY_TAB,
        KEY_PAGEUP, KEY_PAGEDOWN,
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (ioctl(fd, UI_SET_KEYBIT, keys[i]) < 0) {
            perror("joystick_bridge: UI_SET_KEYBIT");
            close(fd);
            return -1;
        }
    }

    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor  = 0x1234;
    usetup.id.product = 0x5678;
    usetup.id.version = 1;
    strncpy(usetup.name, "joystick_bridge virtual kbd",
            UINPUT_MAX_NAME_SIZE - 1);

    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
        perror("joystick_bridge: UI_DEV_SETUP");
        close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("joystick_bridge: UI_DEV_CREATE");
        close(fd);
        return -1;
    }

    return fd;
}

static void uinput_close(int fd)
{
    if (fd >= 0) {
        ioctl(fd, UI_DEV_DESTROY);
        close(fd);
    }
}

/* -----------------------------------------------------------------------
 * Signal / cleanup
 * ----------------------------------------------------------------------- */

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* -----------------------------------------------------------------------
 * Main event loop
 * ----------------------------------------------------------------------- */

static void translate_and_forward(int joy_fd, int ufd,
                                   struct hat_state *hat)
{
    struct input_event ev;
    ssize_t n = read(joy_fd, &ev, sizeof(ev));
    if (n <= 0)
        return;

    if (ev.type == EV_KEY) {
        /* Direct button → key mapping */
        for (size_t i = 0; i < BTN_MAP_LEN; i++) {
            if (ev.code == BTN_MAP[i].btn_code) {
                /* value: 1=press, 0=release, 2=repeat */
                if (ev.value == 1 || ev.value == 0)
                    uinput_key(ufd, BTN_MAP[i].key_code, ev.value);
                return;
            }
        }
        return;
    }

    if (ev.type == EV_ABS) {
        int old_x = hat->x;
        int old_y = hat->y;

        if (ev.code == ABS_HAT0X) {
            hat->x = (ev.value > 0) ? 1 : (ev.value < 0) ? -1 : 0;
        } else if (ev.code == ABS_HAT0Y) {
            hat->y = (ev.value > 0) ? 1 : (ev.value < 0) ? -1 : 0;
        } else {
            return;
        }

        /* Release old direction, press new one */
        if (old_x != 0 && hat->x != old_x) {
            uint16_t rel_key = (old_x < 0) ? KEY_LEFT : KEY_RIGHT;
            uinput_key(ufd, rel_key, 0);
        }
        if (old_y != 0 && hat->y != old_y) {
            uint16_t rel_key = (old_y < 0) ? KEY_UP : KEY_DOWN;
            uinput_key(ufd, rel_key, 0);
        }
        if (hat->x != 0 && hat->x != old_x) {
            uint16_t press_key = (hat->x < 0) ? KEY_LEFT : KEY_RIGHT;
            uinput_key(ufd, press_key, 1);
        }
        if (hat->y != 0 && hat->y != old_y) {
            uint16_t press_key = (hat->y < 0) ? KEY_UP : KEY_DOWN;
            uinput_key(ufd, press_key, 1);
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr,
                "Usage: %s <joystick-event-device> <child-pid>\n"
                "  e.g. %s /dev/input/event2 1234\n",
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    const char *joy_dev   = argv[1];
    pid_t        child_pid = (pid_t)atoi(argv[2]);

    if (child_pid <= 0) {
        fprintf(stderr, "joystick_bridge: invalid child PID: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    /* Open the physical joystick/gamepad event device */
    int joy_fd = open(joy_dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (joy_fd < 0) {
        fprintf(stderr, "joystick_bridge: cannot open %s: %s\n",
                joy_dev, strerror(errno));
        return EXIT_FAILURE;
    }

    /* Grab the device so that the raw events don't reach other consumers
     * (avoids duplicate key-presses if the kernel joystick driver also
     * generates keyboard events for the same device). */
    if (ioctl(joy_fd, EVIOCGRAB, (void *)1) < 0) {
        /* Non-fatal: some devices don't support exclusive grab */
        perror("joystick_bridge: EVIOCGRAB (non-fatal)");
    }

    /* Create uinput virtual keyboard */
    uinput_fd = uinput_open();
    if (uinput_fd < 0) {
        close(joy_fd);
        return EXIT_FAILURE;
    }

    /* Set up clean signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);

    struct hat_state hat = { 0, 0 };

    /*
     * Main loop: forward events while the child process is alive.
     *
     * Use poll() with a 200 ms timeout so we sleep until an input event
     * arrives (or until the timeout fires so we can check the child PID).
     * This avoids busy-waiting while still detecting child exit promptly.
     */
    struct pollfd pfd;
    pfd.fd     = joy_fd;
    pfd.events = POLLIN;

    while (g_running) {
        int rc = poll(&pfd, 1, 200 /* ms */);
        if (rc < 0) {
            if (errno == EINTR)
                continue;   /* interrupted by a signal – re-evaluate g_running */
            perror("joystick_bridge: poll");
            break;
        }

        /* Check if child is still running (after every poll timeout or event) */
        if (kill(child_pid, 0) < 0 && errno == ESRCH) {
            break;
        }

        if (rc > 0 && (pfd.revents & POLLIN)) {
            translate_and_forward(joy_fd, uinput_fd, &hat);
        }
    }

    /* Release any held keys before destroying the uinput device */
    static const uint16_t all_keys[] = {
        KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
        KEY_ENTER, KEY_ESC, KEY_SPACE, KEY_TAB,
        KEY_PAGEUP, KEY_PAGEDOWN,
    };
    for (size_t i = 0; i < sizeof(all_keys) / sizeof(all_keys[0]); i++)
        uinput_key(uinput_fd, all_keys[i], 0);

    /* Release the exclusive grab before closing */
    ioctl(joy_fd, EVIOCGRAB, (void *)0);
    close(joy_fd);
    uinput_close(uinput_fd);

    return EXIT_SUCCESS;
}
