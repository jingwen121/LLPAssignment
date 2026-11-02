#!/usr/bin/env bash
set -euo pipefail
umask 077

# Refuse an existing path, including a symlink; mkdir without -p also closes the
# race where 2 invocations both pass the existence check and overwrite keys
output=${1:-credentials}
if [[ -e "$output" ]]; then
    echo "Directory already exists: $output" >&2
    exit 1
fi
mkdir -- "$output"
output=$(cd "$output" && pwd)
mkdir "$output/authority"

# This is the trust root for this game's identities and policy signatures
# Keep its private key with the administrator; clients receive only ca.pem
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 365 \
    -addext 'basicConstraints=critical,CA:TRUE' \
    -addext 'keyUsage=critical,digitalSignature,keyCertSign,cRLSign' \
    -subj '/CN=CoinRushCA' -keyout "$output/authority/ca.key" \
    -out "$output/authority/ca.pem" 2>/dev/null

# Require authentication and encrypt game data, discovery and RTPS traffic
cat > "$output/authority/governance.xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<dds><domain_access_rules><domain_rule>
  <domains><id>42</id></domains>
  <allow_unauthenticated_participants>false</allow_unauthenticated_participants>
  <enable_join_access_control>true</enable_join_access_control>
  <discovery_protection_kind>ENCRYPT</discovery_protection_kind>
  <liveliness_protection_kind>ENCRYPT</liveliness_protection_kind>
  <rtps_protection_kind>ENCRYPT</rtps_protection_kind>
  <topic_access_rules><topic_rule>
    <topic_expression>*</topic_expression>
    <enable_discovery_protection>true</enable_discovery_protection>
    <enable_liveliness_protection>true</enable_liveliness_protection>
    <enable_read_access_control>true</enable_read_access_control>
    <enable_write_access_control>true</enable_write_access_control>
    <metadata_protection_kind>ENCRYPT</metadata_protection_kind>
    <data_protection_kind>ENCRYPT</data_protection_kind>
  </topic_rule></topic_access_rules>
</domain_rule></domain_access_rules></dds>
XML

# The governance signature protects the encryption requirements; permissions
# below are signed separately so editing allowed topics invalidates the signature
openssl smime -sign -text -md sha256 \
    -in "$output/authority/governance.xml" \
    -signer "$output/authority/ca.pem" -inkey "$output/authority/ca.key" \
    -out "$output/authority/governance.p7s"

# Issue a separate identity for each client; sharing a directory shares its key
# The sixth client is available for the server-full test, not a sixth game slot
for role in server client1 client2 client3 client4 client5 client6; do
    mkdir "$output/$role"
    openssl req -new -newkey rsa:2048 -nodes -sha256 \
        -subj "/CN=$role" -keyout "$output/$role/identity.key" \
        -out "$output/authority/$role.csr" 2>/dev/null
    printf 'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n' \
        > "$output/authority/identity.ext"
    openssl x509 -req -sha256 -days 365 \
        -in "$output/authority/$role.csr" -CA "$output/authority/ca.pem" \
        -CAkey "$output/authority/ca.key" -CAcreateserial \
        -extfile "$output/authority/identity.ext" \
        -out "$output/$role/identity.pem" 2>/dev/null

    # Client code cannot grant itself a GameState writer; Cyclone checks the
    # signed grant before creating that endpoint; unknown topics default to DENY
    publish="<topic>JoinRequest_${role}</topic><topic>Input</topic>"
    subscribe="<topic>JoinResponse_${role}</topic><topic>GameState_${role}</topic>"
    if [[ "$role" == server ]]; then
        publish='<topic>JoinResponse_client?</topic><topic>GameState_client?</topic>'
        subscribe='<topic>JoinRequest_client?</topic><topic>Input</topic>'
    fi

    # Use the exact certificate subject when granting access to each identity
    subject=$(openssl x509 -in "$output/$role/identity.pem" -noout -subject -nameopt RFC2253)
    subject=${subject#subject=}
    # A small validity overlap tolerates setup clock differences; all machines
    # still need sensible clocks because certificate expiry is checked by DDS
    start=$(date -u -d '1 day ago' +%Y-%m-%dT%H:%M:%S)
    finish=$(date -u -d '364 days' +%Y-%m-%dT%H:%M:%S)
    cat > "$output/authority/$role.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<dds><permissions><grant name="$role">
  <subject_name>$subject</subject_name>
  <validity><not_before>${start}Z</not_before><not_after>${finish}Z</not_after></validity>
  <allow_rule><domains><id>42</id></domains>
    <publish><topics>$publish</topics></publish>
    <subscribe><topics>$subscribe</topics></subscribe>
  </allow_rule>
  <default>DENY</default>
</grant></permissions></dds>
XML
    openssl smime -sign -text -md sha256 \
        -in "$output/authority/$role.xml" \
        -signer "$output/authority/ca.pem" -inkey "$output/authority/ca.key" \
        -out "$output/$role/permissions.p7s"
    cp "$output/authority/ca.pem" "$output/authority/governance.p7s" "$output/$role/"
done

echo "Created server and client credentials in $output"
echo 'Give each player only their own client directory. Keep authority and server private.'
