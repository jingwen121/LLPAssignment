CC = gcc
CPPFLAGS += -Ibuild
CFLAGS = -std=c11 -D_DEFAULT_SOURCE -O2 -g -Wall -Wextra -Wpedantic
CYCLONE_CFLAGS = $(shell pkg-config --cflags CycloneDDS)
GLFW_CFLAGS = $(shell pkg-config --cflags glfw3)
CYCLONE_LIBS = $(shell pkg-config --libs CycloneDDS)
GLFW_LIBS = $(shell pkg-config --libs glfw3)
OPENSSL_LIBS = $(shell pkg-config --libs openssl)
SDL_MIXER_CFLAGS = $(shell pkg-config --cflags SDL2_mixer)
SDL_MIXER_LIBS = $(shell pkg-config --libs SDL2_mixer)
DDS_LIBS = $(CYCLONE_LIBS) -pthread $(OPENSSL_LIBS)
EFFECT_ASSETS = assets/effects/bomb_spritesheet.rgba assets/effects/plus_spritesheet.rgba assets/effects/colored_hearts.rgba
SANFLAGS = -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all server client clean check-deps credentials run run-player2 test qa test-sanitize profile wire loss

# `make` prepares a runnable local build. `make run` also starts a local server
# and opens the first client; use separate build/server and build/client
# commands when hosting a network game with multiple client identities.
all: check-deps
	$(MAKE) --no-print-directory build/server build/client credentials server client
	@echo 'Build ready. Launch the local game with: make run'

# Keep the familiar ./server and ./client commands as copies of the build.
# A no-argument client selects the first locally unlocked signed identity.
server: build/server
	@tmp=$$(mktemp ./.server.install.XXXXXX); \
	  trap 'rm -f "$$tmp"' EXIT; \
	  cp build/server "$$tmp" && mv -f "$$tmp" server

client: build/client
	@tmp=$$(mktemp ./.client.install.XXXXXX); \
	  trap 'rm -f "$$tmp"' EXIT; \
	  cp build/client "$$tmp" && mv -f "$$tmp" client

check-deps:
	@if command -v gcc >/dev/null 2>&1 && \
	   command -v pkg-config >/dev/null 2>&1 && \
	   command -v idlc >/dev/null 2>&1 && \
	   command -v openssl >/dev/null 2>&1 && \
	   pkg-config --exists CycloneDDS glfw3 openssl SDL2_mixer && \
	   test -r /usr/include/openssl/pem.h && \
	   test -r /usr/include/GLFW/glfw3.h && \
	   test -r /usr/include/SDL2/SDL_mixer.h && \
	   test -r /usr/include/GL/gl.h; then \
	    echo 'Build dependencies are installed.'; \
	  elif command -v apt-get >/dev/null 2>&1; then \
	    echo 'Installing missing build dependencies with apt...'; \
	    if [ "$$(id -u)" -eq 0 ]; then \
	      apt-get update && \
	      apt-get install -y build-essential pkg-config cyclonedds-dev cyclonedds-tools libglfw3-dev libgl1-mesa-dev libssl-dev libsdl2-mixer-dev; \
	    elif command -v sudo >/dev/null 2>&1; then \
	      sudo apt-get update && \
	      sudo apt-get install -y build-essential pkg-config cyclonedds-dev cyclonedds-tools libglfw3-dev libgl1-mesa-dev libssl-dev libsdl2-mixer-dev; \
	    else \
	      echo 'Cannot install packages: run as root or install sudo.' >&2; exit 1; \
	    fi; \
	    if ! command -v gcc >/dev/null 2>&1 || \
	       ! command -v pkg-config >/dev/null 2>&1 || \
	       ! command -v idlc >/dev/null 2>&1 || \
	       ! command -v openssl >/dev/null 2>&1 || \
	       ! pkg-config --exists CycloneDDS glfw3 openssl SDL2_mixer || \
	       ! test -r /usr/include/openssl/pem.h || \
	       ! test -r /usr/include/GLFW/glfw3.h || \
	       ! test -r /usr/include/SDL2/SDL_mixer.h || \
	       ! test -r /usr/include/GL/gl.h; then \
	        echo 'Dependency installation did not provide all required tools.' >&2; exit 1; \
	    fi; \
	  else \
	    echo 'Missing build dependencies. This auto-installer supports Debian/Ubuntu/WSL; install the packages listed in README.md.' >&2; exit 1; \
	  fi

clean:
	rm -rf build
	rm -f messages.c messages.h server client test_validation *.o core core.*
	rm -f .server.install.* .client.install.*

credentials: security.sh
	@if test -f credentials-private/server/identity.pem && \
	    test -f credentials-private/client1/identity.pem; then \
	    echo 'Local credentials are ready.'; \
	else \
	    if test -e credentials-private; then \
	        echo 'credentials-private exists but is incomplete; move it aside before regenerating.' >&2; exit 1; \
	    fi; \
	    bash security.sh credentials-private; \
	fi

run: all
	@./server & server_pid=$$!; \
	  trap 'kill "$$server_pid" 2>/dev/null || true; wait "$$server_pid" 2>/dev/null || true' EXIT INT TERM; \
	  sleep 1; \
	  ./client

# Open this from a second terminal while `make run` is active. The local lock
# causes its bare client launch to select the next available signed identity.
run-player2: all
	./client

build: | check-deps
	mkdir -p build

build/messages.c build/messages.h &: messages.idl | build
	cd build && idlc -l c ../messages.idl

build/server: server.c game.c game.h network.c network.h build/messages.c build/messages.h
	$(CC) $(CPPFLAGS) $(CYCLONE_CFLAGS) $(CFLAGS) server.c game.c network.c build/messages.c -o $@ $(DDS_LIBS)

build/client: client.c network.c network.h stb_easy_font.h $(EFFECT_ASSETS) assets/music/level1.mp3 assets/music/level2.mp3 build/messages.c build/messages.h
	$(CC) $(CPPFLAGS) $(CYCLONE_CFLAGS) $(GLFW_CFLAGS) $(SDL_MIXER_CFLAGS) $(CFLAGS) client.c network.c build/messages.c -o $@ $(DDS_LIBS) $(GLFW_LIBS) $(SDL_MIXER_LIBS) -lGL -lm

build/test_validation: test_validation.c network.c network.h build/messages.c build/messages.h
	$(CC) $(CPPFLAGS) $(CYCLONE_CFLAGS) $(CFLAGS) test_validation.c network.c build/messages.c -o $@ $(DDS_LIBS)

build/test_game: test_game.c game.c game.h | build
	$(CC) $(CFLAGS) test_game.c game.c -o $@

test: build/test_validation build/test_game
	./build/test_game
	./build/test_validation credentials-private

# Run security and networking checks
qa: all
	python3 qa.py check

# Compile the same process-based tests with runtime memory and UB checks
build/test_sanitized: test_validation.c network.c network.h build/messages.c build/messages.h
	$(CC) $(CPPFLAGS) $(CYCLONE_CFLAGS) $(SANFLAGS) test_validation.c network.c build/messages.c -o $@ $(DDS_LIBS)

build/server_sanitized: server.c game.c game.h network.c network.h build/messages.c build/messages.h
	$(CC) $(CPPFLAGS) $(CYCLONE_CFLAGS) $(SANFLAGS) server.c game.c network.c build/messages.c -o $@ $(DDS_LIBS)

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
