CFLAGS = -Iatop -g -O0 -lz -Wall
OBJS = cache.o httpd.o json.o output.o rawlog.o version.o

all: submodule bin
	gcc -o atophttpd $(CFLAGS) $(OBJS)

bin: $(OBJS)
	gcc -o atophttpd $(CFLAGS) $(OBJS)

%.o: %.c
	$(CC) -c $(CFLAGS) $*.c -o $*.o

version.o:
	@make -C atop versdate.h
	$(CC) -c $(CFLAGS) atop/version.c -o version.o

submodule:
	git submodule update --init --recursive

clean:
	@rm -f atophttpd *.o
