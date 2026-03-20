PROJ = ifbwgraph
CC = gcc
SOURCES = $(wildcard *.c)
OBJS = $(patsubst %.c,%.o,$(SOURCES))
LIBS =
BUILD =
CFLAGS = -Wall -Werror -pedantic -pthread

ifdef DEBUG
  CFLAGS += -ggdb
else
  BUILD = -s
endif

LIBS += $(shell pkg-config --libs libevent_pthreads libpng)
CFLAGS += $(shell pkg-config --cflags  libevent_pthreads libpng)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

$(PROJ): $(OBJS)
	$(CC) $(BUILD) $(OBJS) -o $@ $(LIBS)

clean:
	rm -f $(PROJ) *.o

all: $(PROJ)
