# Makefile for ki25-3a smoke test
# Cross-compiles for RISC-V

CC = riscv64-linux-gnu-gcc
CFLAGS = -O2 -static

TARGET = rbook_test_smoke

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
