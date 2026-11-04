#!/usr/bin/env bash
set -euo pipefail

readonly EXPECTED_CYCLONEDDS_VERSION="0.10.4 or 0.10.5"
role=${1:-}

if [[ "$role" != "server" && "$role" != "client" &&
      ! "$role" =~ ^invite-client[2-6]$ ]]; then
    echo "Usage: bash scripts/run-game-wsl.sh server|client|invite-client2..invite-client6" >&2
    exit 2
fi

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$project_dir"
. /etc/os-release
build_fingerprint="${ID:-linux}:${VERSION_ID:-unknown}:CycloneDDS"

# Bootstrap only the tools needed to run the Makefile on a fresh Debian/Ubuntu
# WSL distro. The Makefile installs the game's remaining development packages.
if ! command -v make >/dev/null 2>&1 || ! command -v pkg-config >/dev/null 2>&1; then
    if ! command -v apt-get >/dev/null 2>&1 || ! command -v sudo >/dev/null 2>&1; then
        echo "Install make and pkg-config (pkgconf) in this WSL distro, then retry." >&2
        exit 1
    fi
    sudo apt-get update
    sudo apt-get install -y build-essential pkgconf
fi

if ! pkg-config --exists CycloneDDS; then
    make check-deps
fi

install_apt_packages() {
    if [[ "$(id -u)" -eq 0 ]]; then
        apt-get update
        apt-get install -y "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo apt-get update
        sudo apt-get install -y "$@"
    else
        echo "Install required package(s) with apt: $*" >&2
        return 1
    fi
}

actual_version=$(pkg-config --modversion CycloneDDS 2>/dev/null || true)
if [[ "$actual_version" != "0.10.4" && "$actual_version" != "0.10.5" ]]; then
    echo "Refusing to launch: Cyclone DDS $EXPECTED_CYCLONEDDS_VERSION is required." >&2
    echo "This WSL distro reports '${actual_version:-not installed}'." >&2
    echo "Choose a WSL distro that provides Cyclone DDS $EXPECTED_CYCLONEDDS_VERSION." >&2
    exit 1
fi

stamp="$project_dir/build/.coinrush-build-fingerprint"
needs_clean=false
if [[ ! -r "$stamp" || "$(<"$stamp")" != "$build_fingerprint $actual_version" ]]; then
    needs_clean=true
elif [[ "$role" == "server" &&
        ( ! -x "$project_dir/server" || ! -x "$project_dir/client" ) ]]; then
    needs_clean=true
elif [[ "$role" == "client" && ! -x "$project_dir/client" ]]; then
    needs_clean=true
fi
if [[ "$needs_clean" == true ]]; then
    echo "Building Coin Rush for ${PRETTY_NAME:-$ID} with Cyclone DDS $actual_version..."
    make clean
fi
if [[ "$role" == "server" || "$role" == invite-client* ]]; then
    make
else
    # A guest only needs the client binary; avoid generating a local CA and
    # server credentials on every friend's machine.
    make build/client client
fi

# Record which DDS release produced this build so a later launcher run using
# another WSL distro forces a clean rebuild instead of reusing stale outputs.
mkdir -p "$project_dir/build"
printf '%s %s\n' "$build_fingerprint" "$actual_version" > "$stamp"

resolved_libdir=$(readlink -f "$(pkg-config --variable=libdir CycloneDDS)")
if [[ "$role" == "server" || "$role" == invite-client* ]]; then
    executables=(server client)
else
    executables=(client)
fi
for executable in "${executables[@]}"; do
    ldd_output=$(ldd "$project_dir/$executable")
    linked_library=$(printf '%s\n' "$ldd_output" |
        awk '$1 ~ /^libddsc[.]so/ { for (i = 1; i <= NF; ++i) if ($i ~ /^\//) { print $i; exit } }')
    if [[ -z "$linked_library" || ! -e "$linked_library" ]]; then
        echo "Could not resolve Cyclone DDS for $executable; ldd reported:" >&2
        printf '%s\n' "$ldd_output" | grep 'libddsc' >&2 || true
        exit 1
    fi

    resolved_library=$(readlink -f "$linked_library")
    case "$resolved_library" in
        "$resolved_libdir"/*) ;;
        *)
            echo "$executable loads Cyclone DDS from an unexpected library path:" >&2
            echo "  $resolved_library" >&2
            echo "Expected a library under $resolved_libdir." >&2
            exit 1
            ;;
    esac
done

if [[ "$role" == invite-client* ]]; then
    command -v gpg >/dev/null 2>&1 || install_apt_packages gnupg
    player=${role#invite-}
    exec bash "$project_dir/scripts/make-invite.sh" "$player"
fi

if [[ "$role" == "server" ]]; then
    : > credentials-private/.coinrush-server-host
    chmod 600 credentials-private/.coinrush-server-host
    echo "Cyclone DDS $actual_version verified; starting the room server."
    exec "$project_dir/server"
fi

# Prefer the per-player signed invite. If the encrypted invite was bundled in
# the project folder, import it on first launch with its separately shared
# passphrase. The authority/server private key is never needed by a client.
data_home=${XDG_DATA_HOME:-${HOME:?HOME is required for private credential storage}/.local/share}
credential_store="$data_home/coinrush/credentials-invites"
if [[ -r "$credential_store/default-player" ]]; then
    player=$(<"$credential_store/default-player")
    if [[ ! "$player" =~ ^[2-6]$ ||
          ! -r "$credential_store/client${player}/identity.key" ]]; then
        echo "The imported client invite is incomplete; import a fresh invite." >&2
        exit 1
    fi
elif [[ -r credentials-invites/default-player ]]; then
    # Continue to honor an invite already imported by an earlier release.
    player=$(<credentials-invites/default-player)
    if [[ ! "$player" =~ ^[2-6]$ ||
          ! -r "credentials-invites/client${player}/identity.key" ]]; then
        echo "The imported client invite is incomplete; import a fresh invite." >&2
        exit 1
    fi
elif [[ -r credentials-private/.coinrush-server-host &&
        -r credentials-private/client1/identity.key ]]; then
    # The computer that launched server.bat plays locally as client1.
    :
elif compgen -G 'invites/coinrush-client*.tar.gz.gpg' >/dev/null; then
    shopt -s nullglob
    invite_files=(invites/coinrush-client*.tar.gz.gpg)
    shopt -u nullglob
    if [[ ${#invite_files[@]} -ne 1 ]]; then
        echo "Keep only this computer's encrypted client invite in invites/." >&2
        exit 1
    fi
    command -v gpg >/dev/null 2>&1 || install_apt_packages gnupg
    unset COINRUSH_AUTO_JOIN
    echo "Importing the encrypted client identity. Enter its invite passphrase."
    exec bash "$project_dir/scripts/join-invite.sh" "${invite_files[0]}"
elif [[ -r credentials-private/client1/identity.key ]]; then
    # Also support a host that started ./server manually instead of server.bat.
    :
else
    echo "No client identity is installed. Place this player's encrypted invite" >&2
    echo "in invites/ and run client.bat again. Never copy the authority or server key." >&2
    exit 1
fi

unset COINRUSH_AUTO_JOIN
echo "Cyclone DDS $actual_version verified; opening Create Room / Join Room."
exec "$project_dir/client"
