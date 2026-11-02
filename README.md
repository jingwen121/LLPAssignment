# Coin Rush

Coin Rush is a small real-time multiplayer game built in C with Cyclone DDS,
GLFW and fixed-function OpenGL. The server owns the simulation: player
movement, falling coins, collision checks, scores and round timing. Each client
sends left/right input and renders the latest complete server snapshot.

## Build in WSL

Install the project dependencies in your WSL distribution (Cyclone DDS,
GLFW 3, OpenGL development headers, GCC and `pkg-config`). From this folder:

```sh
idlc -l c messages.idl
gcc -std=c11 -Wall -Wextra -pthread server.c game.c messages.c \
  $(pkg-config --cflags --libs CycloneDDS) -lm -o server
gcc -std=c11 -Wall -Wextra -pthread client.c messages.c \
  $(pkg-config --cflags --libs glfw3 CycloneDDS) -lGL -lm -o client
```

Run `./server` first, then start `./client` in at least two WSL terminals.
Every client joins automatically. Use the LEFT and RIGHT arrow keys or A and D
to move; closing a client window exits that client.

## Client and graphics (Part B)

- GLFW polls left/right movement and sends the current direction over DDS.
- The server publishes a single `GameState` sample for each simulation tick.
  This includes all active player positions and scores, falling coin positions
  and types, phase, timer and winner data, so the HUD and arena share one
  authoritative update.
- OpenGL draws the arena grid, colored player squares, normal and gold coins,
  player score cards, the waiting room and the winner/results overlay.
- The client keeps DDS work on a worker thread and copies received state under
  a mutex before rendering on the GLFW thread.

The fixed DDS arrays are sized for five players and eight coins, matching the
game logic constants in `game.h`. `messages.c` and `messages.h` are generated
from `messages.idl` and are intentionally not checked in.
