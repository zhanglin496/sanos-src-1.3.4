#
# Makefile for user32 library
#

all: user32.dll

user32.dll: user32.c
    $(CC) -shared -o $@ $^

install: user32.dll
    sh cp user32.dll /bin/user32.dll

clean:
    sh rm user32.dll
