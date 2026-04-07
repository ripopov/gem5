CC = riscv64-linux-gnu-gcc
CFLAGS = -O2 -static

BINS = trivial rbook_test_smoke

all: $(BINS)

trivial: trivial.c
	$(CC) $(CFLAGS) -o $@ $<

rbook_test_smoke: rbook_test_smoke.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BINS)

.PHONY: all clean
