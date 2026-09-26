UC      ?= ../unicorn-src
UCBUILD ?= ../build-uc
CFLAGS  ?= -O2 -g -Wall -Wno-unused-parameter
CFLAGS  += -I$(UC)/include
SRCS     = machine.c pic.c pit.c uart.c i8042.c linuxboot.c host_posix.c
OBJS     = $(SRCS:.c=.o)

x64e: $(OBJS) $(UCBUILD)/libunicorn.a
	$(CC) -o $@ $(OBJS) $(UCBUILD)/libunicorn.a -lpthread -lm

%.o: %.c x64e.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(OBJS) x64e
