#
# Makefile for winmm library
#

all: winmm.dll

winmm.dll: winmm.c
    $(CC) -shared -o $@ $^

install: winmm.dll
    sh cp winmm.dll /bin/winmm.dll

clean:
    sh rm winmm.dll
