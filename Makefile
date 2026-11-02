CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -pthread
CYCLONE_CFLAGS := $(shell pkg-config --cflags CycloneDDS)
CYCLONE_LIBS := $(shell pkg-config --libs CycloneDDS)
GLFW_CFLAGS := $(shell pkg-config --cflags glfw3)
GLFW_LIBS := $(shell pkg-config --libs glfw3)

.PHONY: all clean

all: server client

messages.c messages.h: messages.idl
	idlc -l c messages.idl

server: server.c game.c game.h messages.c messages.h
	$(CC) $(CFLAGS) $(CYCLONE_CFLAGS) server.c game.c messages.c $(CYCLONE_LIBS) -lm -o $@

client: client.c messages.c messages.h stb_easy_font.h
	$(CC) $(CFLAGS) $(GLFW_CFLAGS) $(CYCLONE_CFLAGS) client.c messages.c \
		$(GLFW_LIBS) $(CYCLONE_LIBS) -lGL -lm -o $@

clean:
	rm -f server client messages.c messages.h
