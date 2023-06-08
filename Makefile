CFLAGS = -Iatop -g -O2 -lz -Wall -Wcast-align -std=gnu11
OBJS = cache.o httpd.o json.o output.o rawlog.o version.o connection.o socket.o tls.o
BIN = atophttpd
PREFIX := $(prefix)
CC=gcc

ifneq (,$(filter $(USE_TLS),yes YES y Y 1))
	CFLAGS += -lssl -lcrypto -DUSE_TLS
endif

all: submodule bin
	$(CC) -o $(BIN) $(OBJS) $(CFLAGS)

install: bin
	install -D -s $(BIN) $(PREFIX)/usr/bin/$(BIN)
	install -D atophttpd.service $(PREFIX)/lib/systemd/system/atophttpd.service
	install -D man/atophttpd.1 $(PREFIX)/usr/share/man/man1/atophttpd.1

deb: bin
	@sh packaging/debian/makedeb.sh `pwd`

bin: $(OBJS)
	$(CC) -o $(BIN) $(OBJS) $(CFLAGS)

%.o: %.c
	$(CC) -c $(CFLAGS) $*.c -o $*.o

version.o:
	@make -C atop versdate.h
	$(CC) -c $(CFLAGS) atop/version.c -o version.o

submodule:
	git submodule update --init --recursive

clean:
	@rm -f $(BIN) *.o *.deb
