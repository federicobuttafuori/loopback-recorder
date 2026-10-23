#!/bin/sh
# Cross-compile on Linux with mingw-w64
set -e
cd "$(dirname "$0")/src"
x86_64-w64-mingw32-windres loopback_rec_gui.rc -O coff -o res.o
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -municode -mwindows -static -s -o loopback_rec_gui.exe loopback_rec_gui.c res.o -lole32 -lavrt -lshell32 -luser32 -lgdi32 -lcomctl32 -luuid -lmfuuid
echo "built src/loopback_rec_gui.exe"
