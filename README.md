# Coin Rush

Coin Rush is a small real-time multiplayer game in C using GLFW, fixed-function
OpenGL and Cyclone DDS. The server owns the simulation. Clients use room codes
to create or join isolated matches.

## Build in WSL

Install dependencies once:

```sh
sudo apt update
sudo apt install build-essential pkg-config cyclonedds-dev cyclonedds-tools libglfw3-dev libgl1-mesa-dev libssl-dev
```

From the project directory, build both executables with one command:

```sh
make
```

This generates the DDS message code from `messages.idl`, then creates the
`build/server` and `build/client` executables. The build reuses unchanged files.

## Start a match

Generate credentials once with `bash security.sh credentials-private`; give each player
only their own client directory and keep the authority and server keys private.

1. In one WSL terminal, run `./build/server credentials-private/server`.
2. In a second terminal, run `./build/client credentials-private/client1` and choose **Play - Create Room**.
3. Share the six-digit room code shown in the lobby.
4. Other players run `./build/client credentials-private/client2` with their own credential directory, choose **Join a Room**, enter the code and
   press Enter (or click **Join Room**).

The server supports up to eight rooms with up to five players in each room.
At least two players start a round. Close a client window to leave its room.
Clients must be able to discover the same Cyclone DDS server on the network.

## Controls

- Use the left/right arrow keys or A/D to move.
- Press F11 to enter or leave fullscreen; Escape still opens the in-game exit menu.
- Resize the client window freely; the arena keeps its 4:3 layout and adds
  letterboxing rather than stretching or cropping the game.
- In the room-code screen, type six digits, press Enter to join, Backspace to
  edit, or Escape to return to the main menu.
- Use **Exit Game** in the main menu, or press Escape during a match, to open
  the exit confirmation. Confirming closes the client and removes that player
  from the room; cancel returns to the previous screen.

## Graphics and game screens

- The main menu offers room creation and room-code joining.
- Two menu cats use randomized idle, walk and jump animations from
  `assets/menu/tofu_idle.png`, `cato_walk.png` and `cato_jump.png`. Each client
  gets separate starting positions and animation timing. They bounce inside
  bounded side lanes so they stay on screen and clear of the menu buttons.
- The lobby waits for the second player and displays the room code to share.
- The winner screen shows the winner, top score and results countdown.
- Players are rendered with the cat rotation sprites in `assets/cats/idle`;
  each player has a separate tint and faces left or right as they move.
- The HUD, falling coins, scores, timer and player positions use the latest
  authoritative room snapshot from the server.
- Players can pass through one another. Coins are clipped to the playfield below
  the HUD, including after the window is resized.
- On WSLg, the client uses Mesa software OpenGL with two worker threads and
  synchronized frame swaps to avoid the Intel Arc D3D12 black-frame issue.

Secure channels use the certificate identities client1 through client6; each
identity may hold 1 active player slot across all rooms and attempt at most
5 joins per second. Give each player a distinct credential directory.

After updating from shared DDS topics, generate a new credential set in a new
folder with `bash security.sh credentials-private`; old signed permissions cannot
access the private channels. Start the server with
`./build/server credentials-private/server` and each client with its corresponding
`./build/client credentials-private/client1` directory. Keep the authority private.
