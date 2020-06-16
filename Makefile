CC=gcc
LD=gcc
CFLAGS=-pedantic -Wall -Wextra
LDFLAGS=-g
RM=rm -f

.PHONY: all clean

all: ocrond

clean:
	$(RM) ocrond *.o

ocrond: ocrond.o
	$(LD) $(LDFLAGS) ocrond.o -o $@

ocrond.o: ocrond.c
	$(CC) $(CFLAGS) $(LDFLAGS) -c ocrond.c -o $@
