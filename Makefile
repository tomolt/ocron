# See LICENSE file for copyright and license details.

include config.mk

.PHONY: all clean

all: ocrond

clean:
	rm -f ocrond *.o

ocrond: ocrond.o
	$(LD) $(LDFLAGS) ocrond.o -o $@

ocrond.o: ocrond.c config.h
	$(CC) $(CFLAGS) $(LDFLAGS) -c ocrond.c -o $@

config.h: config.def.h
	cp config.def.h $@
