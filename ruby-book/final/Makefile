CC = riscv64-linux-gnu-gcc
CFLAGS = -O2 -static

BINS = trivial

all: $(BINS)

trivial: trivial.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BINS)

.PHONY: all clean
