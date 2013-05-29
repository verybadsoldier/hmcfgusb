CFLAGS=-MMD -O2 -Wall -I/opt/local/include -g
LDFLAGS=-L/opt/local/lib -lusb-1.0 -lm
CC=gcc

OBJS=hmcfgusb.o hmland.o

all: hmland

DEPEND=$(OBJS:.o=.d)
-include $(DEPEND)

hmland: $(OBJS)

clean:
	rm -f $(OBJS) $(DEPEND) hmland

.PHONY: all clean
