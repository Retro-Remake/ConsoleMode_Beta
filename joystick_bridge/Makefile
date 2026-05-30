# Makefile for joystick_bridge
#
# Cross-compiles for MiSTer (ARM Cortex-A9 hard-float) by default.
# Requires an arm-linux-gnueabihf toolchain installed on the build host.
#
# Targets:
#   make              - cross-compile for MiSTer (ARM)
#   make native       - compile for the current host (x86-64 Linux, for testing)
#   make clean        - remove build artefacts

CC_CROSS ?= arm-linux-gnueabihf-gcc
CC_NATIVE ?= gcc
CFLAGS   := -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE
TARGET   := joystick_bridge
SRC      := joystick_bridge.c

.PHONY: all native clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC_CROSS) $(CFLAGS) -o $@ $^

native: $(SRC)
	$(CC_NATIVE) $(CFLAGS) -o $(TARGET) $^

clean:
	rm -f $(TARGET)
