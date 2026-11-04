#!/usr/bin/env bash
set -euo pipefail
umask 077

archive=${1:-}
if [[ -z "$archive" || ! -f "$archive" ]]; then
    echo "Usage: bash join-invite.sh /path/to/coinrush-clientN.tar.gz.gpg" >&2
    exit 2
fi
command -v gpg >/dev/null || { echo 'Install GnuPG first: sudo apt install gnupg' >&2; exit 1; }
command -v tar >/dev/null || { echo 'tar is required to import the invite.' >&2; exit 1; }
command -v openssl >/dev/null || { echo 'OpenSSL is required to verify the invite.' >&2; exit 1; }

tmp=$(mktemp -d "${TMPDIR:-/tmp}/coinrush-invite.XXXXXX")
trap 'rm -rf -- "$tmp"' EXIT
read -r -s -p 'Invite passphrase: ' passphrase </dev/tty
printf '\n' >&2
exec 3<<<"$passphrase"
unset passphrase
gpg --batch --yes --pinentry-mode loopback --passphrase-fd 3 \
    --decrypt --output "$tmp/invite.tar.gz" -- "$archive"
exec 3<&-

tar -tzf "$tmp/invite.tar.gz" > "$tmp/contents.txt"
player=''
while IFS= read -r entry; do
    if [[ "$entry" =~ ^(client[2-6])/$ ]]; then
        if [[ -n "$player" && "$player" != "${BASH_REMATCH[1]}" ]]; then
            echo 'Invite contains more than one player identity.' >&2
            exit 1
        fi
        player=${BASH_REMATCH[1]}
    fi
done < "$tmp/contents.txt"
if [[ -z "$player" ]]; then
    echo 'Invite archive has no valid client identity.' >&2
    exit 1
fi

expected="$tmp/expected.txt"
printf '%s\n' "$player/" "$player/identity.pem" "$player/identity.key" \
    "$player/ca.pem" "$player/permissions.p7s" "$player/governance.p7s" | LC_ALL=C sort > "$expected"
LC_ALL=C sort "$tmp/contents.txt" > "$tmp/sorted.txt"
if ! cmp -s "$expected" "$tmp/sorted.txt"; then
    echo 'Invite contents do not match the expected signed client credential files.' >&2
    exit 1
fi

mkdir "$tmp/extracted"
tar --no-same-owner --no-same-permissions -xzf "$tmp/invite.tar.gz" -C "$tmp/extracted"
credential_dir="$tmp/extracted/$player"
for file in identity.pem identity.key ca.pem permissions.p7s governance.p7s; do
    if [[ ! -f "$credential_dir/$file" || -L "$credential_dir/$file" ]]; then
        echo "Invite is missing a regular $file credential file." >&2
        exit 1
    fi
done
if find "$credential_dir" -mindepth 1 -maxdepth 1 ! -type f -print -quit | grep -q .; then
    echo 'Invite contains an unexpected file type.' >&2
    exit 1
fi

subject=$(openssl x509 -in "$credential_dir/identity.pem" -noout -subject -nameopt RFC2253)
if [[ "$subject" != "subject=CN=$player" ]] ||
   ! openssl verify -CAfile "$credential_dir/ca.pem" "$credential_dir/identity.pem" >/dev/null 2>&1 ||
   ! openssl smime -verify -in "$credential_dir/permissions.p7s" -CAfile "$credential_dir/ca.pem" \
        -purpose any -out /dev/null >/dev/null 2>&1 ||
   ! openssl smime -verify -in "$credential_dir/governance.p7s" -CAfile "$credential_dir/ca.pem" \
        -purpose any -out /dev/null >/dev/null 2>&1; then
    echo 'Invite certificate or signed DDS permissions failed verification.' >&2
    exit 1
fi

mkdir -p credentials-invites
chmod 700 credentials-invites
destination="credentials-invites/$player"
if [[ -e "$destination" ]]; then
    echo "$destination already exists; remove the old invite before importing another." >&2
    exit 1
fi
chmod 700 "$credential_dir"
chmod 600 "$credential_dir"/*
mv -- "$credential_dir" "$destination"
printf '%s\n' "${player#client}" > credentials-invites/.default-player.tmp
chmod 600 credentials-invites/.default-player.tmp
mv -f -- credentials-invites/.default-player.tmp credentials-invites/default-player

printf 'Imported %s. The next bare ./client launch will use this invited identity.\n' "$player"
if [[ -x ./client ]]; then
    exec ./client
fi
echo 'Run make to build ./client, then launch it with ./client.'
