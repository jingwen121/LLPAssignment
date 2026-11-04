#!/usr/bin/env bash
set -euo pipefail
umask 077

player=${1:-}
if [[ ! "$player" =~ ^client[2-6]$ ]]; then
    echo "Usage: bash make-invite.sh client2|client3|client4|client5|client6" >&2
    exit 2
fi
command -v gpg >/dev/null || { echo 'Install GnuPG first: sudo apt install gnupg' >&2; exit 1; }
command -v tar >/dev/null || { echo 'tar is required to package the invite.' >&2; exit 1; }

credential_dir="credentials-private/$player"
for file in identity.pem identity.key ca.pem permissions.p7s governance.p7s; do
    if [[ ! -f "$credential_dir/$file" ]]; then
        echo "Missing $credential_dir/$file. Run make first." >&2
        exit 1
    fi
done

subject=$(openssl x509 -in "$credential_dir/identity.pem" -noout -subject -nameopt RFC2253)
if [[ "$subject" != "subject=CN=$player" ]]; then
    echo "The certificate in $credential_dir is not the signed $player identity." >&2
    exit 1
fi

# Do not export an identity currently held by a local client process.
exec 9>"/tmp/coin-rush-$(id -u)-$player.lock"
if ! flock -n 9; then
    echo "$player is currently in use. Close that client and choose another identity." >&2
    exit 1
fi

mkdir -p invites
chmod 700 invites
output="invites/coinrush-$player.tar.gz.gpg"
if [[ -e "$output" ]]; then
    echo "$output already exists; move it before creating a replacement invite." >&2
    exit 1
fi

read -r -s -p 'Set an invite passphrase (12+ characters): ' first </dev/tty
printf '\n' >&2
read -r -s -p 'Enter it again: ' second </dev/tty
printf '\n' >&2
if [[ ${#first} -lt 12 || "$first" != "$second" ]]; then
    echo 'The passphrases must match and contain at least 12 characters.' >&2
    exit 1
fi

exec 3<<<"$first"
unset first second
if ! tar -C credentials-private -czf - "$player" |
    gpg --batch --yes --pinentry-mode loopback --passphrase-fd 3 \
        --symmetric --cipher-algo AES256 --output "$output"; then
    rm -f -- "$output"
    echo 'Could not create the encrypted invite.' >&2
    exit 1
fi
exec 3<&-
chmod 600 "$output"
printf 'Created %s. Send the file privately and give its passphrase through a separate channel.\n' "$output"
