CC = gcc
CPPFLAGS += -Ibuild $(shell pkg-config --cflags CycloneDDS glfw3)
CFLAGS = -std=c11 -D_DEFAULT_SOURCE -O2 -g -Wall -Wextra -Wpedantic
DDS_LIBS = $(shell pkg-config --libs CycloneDDS) -pthread
SANFLAGS = -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test qa test-sanitize profile wire loss
all: build/server build/client build/test_validation build/test_game

build:
	mkdir -p build

build/messages.c build/messages.h &: messages.idl | build
	idlc -l c -o build messages.idl

build/server: server.c game.c game.h network.c network.h build/messages.c build/messages.h
	$(CC) $(CPPFLAGS) $(CFLAGS) server.c game.c network.c build/messages.c -o $@ $(DDS_LIBS)

build/client: client.c network.c network.h stb_easy_font.h build/messages.c build/messages.h
	$(CC) $(CPPFLAGS) $(CFLAGS) client.c network.c build/messages.c -o $@ $(DDS_LIBS) $(shell pkg-config --libs glfw3) -lGL -lm

build/test_validation: test_validation.c network.c network.h build/messages.c build/messages.h
	$(CC) $(CPPFLAGS) $(CFLAGS) test_validation.c network.c build/messages.c -o $@ $(DDS_LIBS)

build/test_game: test_game.c game.c game.h | build
	$(CC) $(CFLAGS) test_game.c game.c -o $@

test: build/test_validation build/test_game
	./build/test_game
	./build/test_validation credentials

# Run security and networking checks
qa: all
	python3 qa.py check

# Compile the same process-based tests with runtime memory and UB checks
build/test_sanitized: test_validation.c network.c network.h build/messages.c build/messages.h
	$(CC) $(CPPFLAGS) $(SANFLAGS) test_validation.c network.c build/messages.c -o $@ $(DDS_LIBS)

build/server_sanitized: server.c game.c game.h network.c network.h build/messages.c build/messages.h
	$(CC) $(CPPFLAGS) $(SANFLAGS) server.c game.c network.c build/messages.c -o $@ $(DDS_LIBS)

build/test_game_sanitized: test_game.c game.c game.h | build
	$(CC) $(SANFLAGS) test_game.c game.c -o $@

test-sanitize: build/test_sanitized build/server_sanitized build/test_game_sanitized
	python3 qa.py sanitize

profile: all
	python3 qa.py profile

# Capture and impairment need Linux network privileges; unshare creates a fresh
# network namespace; these commands never apply loss to the host's real network
wire: all
	sudo unshare --net python3 qa.py wire

loss: all
	sudo unshare --net python3 qa.py loss
