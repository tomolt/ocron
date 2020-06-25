# Customize below to fit your system.

# compiler and linker
CC = cc
LD = cc

# build flags
# NOTE GCC with -pedantic gives false positive warnings about syslog().
CFLAGS = -Os -Wall -Wextra
LDFLAGS = -Os

# installation paths
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

