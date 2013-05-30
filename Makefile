CFLAGS=-MMD -O2 -Wall -I/opt/local/include -g
LDFLAGS=-L/opt/local/lib -lusb-1.0 -lm
CC=gcc

HMLAN_OBJS=hmcfgusb.o hmland.o
HMSNIFF_OBJS=hmcfgusb.o hmsniff.o

OBJS=$(HMLAN_OBJS) $(HMSNIFF_OBJS)

all: hmland hmsniff

DEPEND=$(OBJS:.o=.d)
-include $(DEPEND)

hmland: $(HMLAN_OBJS)

hmsniff: $(HMSNIFF_OBJS)

clean:
	rm -f $(HMLAN_OBJS) $(HMSNIFF_OBJS) $(DEPEND) hmland hmsniff

.PHONY: all clean
