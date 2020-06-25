# See LICENSE file for copyright and license details.

include config.mk

.PHONY: all clean install uninstall

all: ocrond

clean:
	rm -f ocrond ocrond.o

install: ocrond
	# ocrond
	mkdir -p "$(DESTDIR)$(PREFIX)/bin"
	cp -f ocrond "$(DESTDIR)$(PREFIX)/bin"
	chmod 755 "$(DESTDIR)$(PREFIX)/bin/ocrond"
	# ocron.1
	mkdir -p "$(DESTDIR)$(MANPREFIX)/man1"
	cp -f ocron.1 "$(DESTDIR)$(MANPREFIX)/man1"
	chmod 644 "$(DESTDIR)$(MANPREFIX)/man1/ocron.1"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/ocrond"

ocrond: ocrond.o
	$(LD) $(LDFLAGS) ocrond.o -o $@

ocrond.o: ocrond.c config.h
	$(CC) $(CFLAGS) $(LDFLAGS) -c ocrond.c -o $@

config.h: config.def.h
	cp config.def.h $@
