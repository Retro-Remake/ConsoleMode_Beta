# joystick_bridge

A small utility that translates gamepad/joystick Linux input events into
keyboard events via the kernel `uinput` interface, so that terminal programs
(e.g. `dialog`, `ini_settings`) can be navigated with a wireless gamepad even
after the SDL parent process (ConsoleMode) has exited.

---

## Problem

ConsoleMode is an SDL application that reads gamepad input through SDL's
joystick subsystem.  When it forks a child process to run a `dialog`-based
menu (e.g. `ini_settings`), two things happen:

1. The SDL parent exits – releasing its SDL joystick context.
2. The child (`dialog`) is a terminal/ncurses program that understands only
   keyboard input from the TTY.  It has no way to read from a joystick event
   device directly.

Result: the gamepad stops working as soon as the SDL parent has gone away.

---

## Solution

`joystick_bridge` sits between the physical gamepad and the dialog subprocess:

```
[gamepad /dev/input/eventN]
         │  (EV_KEY / EV_ABS events)
         ▼
 joystick_bridge  ──(uinput)──►  [virtual keyboard]
                                          │
                                          ▼
                               dialog / ini_settings
                             (reads arrow keys, Enter, Esc)
```

The bridge:
* Opens the physical joystick event device.
* Grabs it exclusively so raw events are not double-delivered.
* Creates a `uinput` virtual keyboard device.
* Forwards translated key events to that virtual keyboard.
* Monitors the dialog child PID and exits automatically when the child
  terminates, releasing the grab and destroying the virtual device.

---

## Default key mapping

| Gamepad input        | Keyboard event |
|----------------------|----------------|
| D-pad Up / HAT0Y-    | ↑ Arrow Up     |
| D-pad Down / HAT0Y+  | ↓ Arrow Down   |
| D-pad Left / HAT0X-  | ← Arrow Left   |
| D-pad Right / HAT0X+ | → Arrow Right  |
| Button A (South)     | Enter          |
| Button B (East)      | Escape         |
| Button X (West)      | Space          |
| Button Y (North)     | Tab            |
| L1 (Left shoulder)   | Page Up        |
| R1 (Right shoulder)  | Page Down      |
| Select / Back        | Escape         |
| Start                | Enter          |

---

## Building

### Cross-compile for MiSTer (ARM Cortex-A9)

Install the ARM cross-compiler on Ubuntu/Debian:

```bash
sudo apt-get install gcc-arm-linux-gnueabihf
```

Then build:

```bash
cd joystick_bridge
make
```

The resulting `joystick_bridge` binary can be copied to the MiSTer SD card
(e.g. `/media/fat/linux/joystick_bridge`).

### Native build (host machine, for testing)

```bash
cd joystick_bridge
make native
```

---

## Usage (shell)

```bash
# Launch dialog and capture its PID
dialog --menu "Settings" 20 60 10 \
    1 "Option A" \
    2 "Option B" \
    2>/tmp/dialog_result &
DIALOG_PID=$!

# Start the bridge for the duration of the dialog
joystick_bridge /dev/input/event2 $DIALOG_PID &

wait $DIALOG_PID
```

To discover the correct event device for your gamepad:

```bash
cat /proc/bus/input/devices | grep -A 5 "Joystick\|Gamepad\|Controller"
# or
ls /dev/input/by-id/ | grep -i joy
```

---

## Integration into ConsoleMode (C example)

Replace the existing subprocess launch code in ConsoleMode with the following
pattern.  This ensures the bridge is started **before** the SDL program exits,
so there is no window where the gamepad is unresponsive.

```c
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * launch_dialog_with_bridge()
 *
 * Forks and exec-s `dialog` (or any terminal subprocess), then immediately
 * starts joystick_bridge so that gamepad input is forwarded to the dialog
 * process for its entire lifetime.
 *
 * @param dialog_argv  NULL-terminated argv for the dialog program.
 * @param joystick_dev Path to the gamepad event device, e.g. "/dev/input/event2".
 * @param bridge_path  Path to the joystick_bridge binary.
 * @return             Exit status returned by dialog, or -1 on error.
 */
int launch_dialog_with_bridge(char *const dialog_argv[],
                               const char *joystick_dev,
                               const char *bridge_path)
{
    /* --- 1. Fork the dialog child ---------------------------------------- */
    pid_t dialog_pid = fork();
    if (dialog_pid < 0) {
        perror("fork dialog");
        return -1;
    }

    if (dialog_pid == 0) {
        /* Child: exec dialog */
        execvp(dialog_argv[0], dialog_argv);
        perror("execvp dialog");
        _exit(127);
    }

    /* --- 2. Fork the joystick_bridge process ----------------------------- */
    pid_t bridge_pid = fork();
    if (bridge_pid < 0) {
        perror("fork joystick_bridge");
        /* Not fatal – dialog still works with keyboard */
    } else if (bridge_pid == 0) {
        /* Child: exec joystick_bridge */
        char pid_str[32];
        snprintf(pid_str, sizeof(pid_str), "%d", (int)dialog_pid);
        execl(bridge_path, "joystick_bridge", joystick_dev, pid_str,
              (char *)NULL);
        perror("execl joystick_bridge");
        _exit(127);
    }

    /* --- 3. Parent (SDL / ConsoleMode) can now exit or wait -------------- */
    /*
     * If ConsoleMode needs to exit here, simply return after the forks.
     * joystick_bridge monitors dialog_pid internally and exits when done.
     *
     * If ConsoleMode should wait for dialog to finish before continuing:
     */
    int status = 0;
    waitpid(dialog_pid, &status, 0);

    /* Wait for the bridge process to finish its cleanup (release grab,
     * destroy uinput device) before returning.  This prevents the bridge
     * from becoming a zombie and ensures the uinput device is gone before
     * ConsoleMode re-initialises SDL's joystick subsystem. */
    if (bridge_pid > 0)
        waitpid(bridge_pid, NULL, 0);

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
```

> **Note:** call `SDL_QuitSubSystem(SDL_INIT_JOYSTICK)` (or `SDL_Quit()`) in
> the SDL parent **before** calling `launch_dialog_with_bridge()`.  This
> releases SDL's grab on the joystick device so that `joystick_bridge` can
> open and exclusively grab it without conflict.

---

## Requirements

* Linux kernel ≥ 4.5 with `CONFIG_INPUT_UINPUT=y` (standard on MiSTer).
* Read access to the joystick event device (typically granted by the `input`
  group or by running as root on MiSTer).
* Write access to `/dev/uinput` (writable by root on MiSTer by default).

---

## License

MIT
