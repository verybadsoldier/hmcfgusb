CFLAGS=-MMD -O2 -Wall -I/opt/local/include -g
LDFLAGS=-L/opt/local/lib
LDLIBS=-lusb-1.0 -lrt
CC=gcc

HMLAN_OBJS=hmcfgusb.o hmland.o
HMSNIFF_OBJS=hmcfgusb.o hmsniff.o
FLASH_HMCFGUSB_OBJS=hmcfgusb.o flash-hmcfgusb.o

OBJS=$(HMLAN_OBJS) $(HMSNIFF_OBJS) $(FLASH_HMCFGUSB_OBJS)

all: hmland hmsniff flash-hmcfgusb

DEPEND=$(OBJS:.o=.d)
-include $(DEPEND)

hmland: $(HMLAN_OBJS)

hmsniff: $(HMSNIFF_OBJS)

flash-hmcfgusb: $(FLASH_HMCFGUSB_OBJS)

clean:
	rm -f $(HMLAN_OBJS) $(HMSNIFF_OBJS) $(FLASH_HMCFGUSB_OBJS) $(DEPEND) hmland hmsniff flash-hmcfgusb

.PHONY: all clean
