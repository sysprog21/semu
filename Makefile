CFLAGS := \
	-O2 -g -Wall -Wextra
CFLAGS += -include common.h

BIN = semu
all: $(BIN) minimal.dtb

OBJS := \
	riscv.o \
	ram.o \
	plic.o \
	uart.o \
	virtio-net.o \
	main.o

deps := $(OBJS:%.o=.%.o.d)

$(BIN): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c -MMD -MF .$@.d $<

DTC ?= dtc

minimal.dtb: minimal.dts
	$(DTC) $< > $@

clean:
	$(RM) $(BIN) $(OBJS) $(deps)

distclean: clean
	$(RM) minimal.dtb

-include $(deps)
