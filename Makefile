CFLAGS=-MMD -O2 -Wall -I/opt/local/include -g
LDFLAGS=-L/opt/local/lib
LDLIBS=-lusb-1.0 -lrt
CC=gcc

HMLAN_OBJS=hmcfgusb.o hmland.o util.o
HMSNIFF_OBJS=hmcfgusb.o hmsniff.o
FLASH_HMCFGUSB_OBJS=hmcfgusb.o firmware.o util.o flash-hmcfgusb.o
FLASH_OTA_OBJS=hmcfgusb.o culfw.o firmware.o util.o flash-ota.o

OBJS=$(HMLAN_OBJS) $(HMSNIFF_OBJS) $(FLASH_HMCFGUSB_OBJS) $(FLASH_OTA_OBJS)

all: hmland hmsniff flash-hmcfgusb flash-ota

DEPEND=$(OBJS:.o=.d)
-include $(DEPEND)

hmland: $(HMLAN_OBJS)

hmsniff: $(HMSNIFF_OBJS)

flash-hmcfgusb: $(FLASH_HMCFGUSB_OBJS)

flash-ota: $(FLASH_OTA_OBJS)

clean:
	rm -f $(HMLAN_OBJS) $(HMSNIFF_OBJS) $(FLASH_HMCFGUSB_OBJS) $(FLASH_OTA_OBJS) $(DEPEND) hmland hmsniff flash-hmcfgusb flash-ota

.PHONY: all clean
