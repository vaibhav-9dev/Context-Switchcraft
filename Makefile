CC = gcc
CFLAGS = -O2 -g -Wall -Wextra -Iinclude -fcf-protection=none -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-omit-frame-pointer
AS = as

all: libcontextchaos.a

libcontextchaos.a: fiber.o context_switch.o
	ar rcs $@ $^

fiber.o: fiber.c include/fiber.h
	$(CC) $(CFLAGS) -c fiber.c -o fiber.o

context_switch.o: context_switch.s
	$(AS) -o context_switch.o context_switch.s

clean:
	rm -f *.o *.a

.PHONY: all clean
