CFLAGS = -Iatop -g -O2 -lz -Wall -std=gnu11
OBJS = cache.o httpd.o json.o output.o rawlog.o version.o
BIN = atophttpd
PREFIX := $(prefix)

all: submodule bin
	gcc -o $(BIN) $(OBJS) $(CFLAGS)

install: bin
	install -D -s $(BIN) $(PREFIX)/usr/bin/$(BIN)
	install -D atophttpd.service $(PREFIX)/lib/systemd/system/atophttpd.service
	install -D man/atophttpd.1 $(PREFIX)/usr/share/man/man1/atophttpd.1

deb: bin
	@sh packaging/debian/makedeb.sh `pwd`

bin: $(OBJS)
	gcc -o $(BIN) $(OBJS) $(CFLAGS)

%.o: %.c
	$(CC) -c $(CFLAGS) $*.c -o $*.o

version.o:
	@make -C atop versdate.h
	$(CC) -c $(CFLAGS) atop/version.c -o version.o

submodule:
	git submodule update --init --recursive

clean:
	@rm -f $(BIN) *.o *.deb
