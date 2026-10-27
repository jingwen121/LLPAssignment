#!/usr/bin/env python3
"""Repeatable Linux security and networking checks"""

import contextlib
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parent
WORK = ROOT / "build" / "qa"
TRUSTED = WORK / "trusted"
FOREIGN = WORK / "foreign"

# These processes need to discover one another, not another group's game on the
# LAN; explicit unicast peers also make the tests usable inside a network namespace
LOCAL_DDS = """<CycloneDDS><Domain Id="42"><General><Interfaces>
<NetworkInterface name="lo" multicast="false"/></Interfaces>
<AllowMulticast>false</AllowMulticast></General><Discovery>
<ParticipantIndex>auto</ParticipantIndex><Peers><Peer Address="127.0.0.1"/></Peers>
</Discovery></Domain></CycloneDDS>"""


def require(condition, explanation):
    # Do not use Python assert for external checks; python -O must still fail QA
    if not condition:
        raise RuntimeError(explanation)


@contextlib.contextmanager
def running(args, log_name, env=None):
    # A separate process group contains forked DDS test children as well; on a
    # timeout or failed check, terminate that whole group so later tests start clean
    with (WORK / log_name).open("w") as log:
        process = subprocess.Popen([str(arg) for arg in args], cwd=ROOT, env=env,
                                   stdout=log, stderr=log, start_new_session=True)
        try:
            yield process
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


def run(args, log_name, expected=0, timeout=60, env=None):
    with running(args, log_name, env) as process:
        status = process.wait(timeout=timeout)
    allowed = (expected,) if isinstance(expected, int) else expected
    require(status in allowed,
            f"{log_name}: expected exit {expected}, got {status}; read {WORK / log_name}")
    print(f"PASS {log_name}", flush=True)
    return (WORK / log_name).read_text()


def prepare():
    WORK.mkdir(parents=True, exist_ok=True)
    # Reuse only this harness's credentials; security.sh refuses replacement, so
    # rerunning QA cannot silently invalidate the real team's certificate set
    for directory in (TRUSTED, FOREIGN):
        if not directory.exists():
            run(["bash", "security.sh", directory], f"create-{directory.name}.log")
    os.environ["CYCLONEDDS_URI"] = LOCAL_DDS
    versions = subprocess.check_output(
        ["pkg-config", "--modversion", "CycloneDDS", "glfw3"], text=True).splitlines()
    (WORK / "environment.json").write_text(json.dumps({
        "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "platform": os.uname().release,
        "cyclonedds": versions[0], "glfw": versions[1],
        "gcc": subprocess.check_output(["gcc", "-dumpfullversion"], text=True).strip(),
    }, indent=2) + "\n")


def checks():
    # Exercise generation on a fresh path as well as reusing earlier QA keys
    # A path with spaces catches shell-quoting mistakes before a teammate hits one
    with tempfile.TemporaryDirectory(prefix="issuance-", dir=WORK) as temporary:
        directory = Path(temporary) / "keys with spaces"
        run(["bash", "security.sh", directory], "fresh-issuance.log")
        ca = directory / "authority" / "ca.pem"
        run(["openssl", "verify", "-CAfile", ca, directory / "server" / "identity.pem",
             directory / "client1" / "identity.pem"], "certificate-chain.log")
        for policy in ("governance", "permissions"):
            # DDS policy signatures are not email certificates; verify trust and
            # signature without imposing OpenSSL's default S/MIME email purpose
            run(["openssl", "smime", "-verify", "-purpose", "any",
                 "-in", directory / "client1" / f"{policy}.p7s",
                 "-CAfile", ca, "-out", Path(temporary) / f"{policy}.xml"],
                f"signed-{policy}.log")
        with running(["./build/server", directory / "server"], "fresh-server.log"):
            run(["./build/test_validation", "--probe", directory / "client1"],
                "fresh-client.log")
    run(["./build/test_validation", TRUSTED], "integration.log")
    run(["./build/server", WORK / "missing"], "missing-server.log", expected=1)
    run(["./build/test_validation", "--probe", WORK / "missing"],
        "missing-client.log", expected=1)

    # An existing output path must fail without changing any private key bytes
    original = (TRUSTED / "authority" / "ca.key").read_bytes()
    run(["bash", "security.sh", TRUSTED], "no-overwrite.log", expected=1)
    require((TRUSTED / "authority" / "ca.key").read_bytes() == original,
            "Credential generator changed an existing CA key")

    # Make independent copies so tampering tests never alter usable credentials
    with tempfile.TemporaryDirectory(prefix="tamper-", dir=WORK) as temporary:
        temporary = Path(temporary)
        for label, filename, old, new in (
                ("permissions", "permissions.p7s", b"<topic>Input</topic>",
                 b"<topic>GameState</topic>"),
                ("governance", "governance.p7s", b"ENCRYPT", b"NONE")):
            directory = temporary / label
            shutil.copytree(TRUSTED / "client1", directory)
            path = directory / filename
            content = path.read_bytes()
            require(old in content, f"No policy text found for {label} tamper test")
            path.write_bytes(content.replace(old, new, 1))
            run(["./build/test_validation", "--probe", directory],
                f"tampered-{label}.log", expected=1)

        directory = temporary / "wrong-key"
        shutil.copytree(TRUSTED / "client1", directory)
        shutil.copyfile(TRUSTED / "client2" / "identity.key", directory / "identity.key")
        # Cyclone versions may detect a key mismatch during startup or during
        # peer authentication; both must reject; a successful probe is forbidden
        run(["./build/test_validation", "--probe", directory],
            "wrong-private-key.log", expected=(1, 2))

        directory = temporary / "missing-key"
        shutil.copytree(TRUSTED / "client1", directory)
        (directory / "identity.key").unlink()
        run(["./build/test_validation", "--probe", directory],
            "missing-private-key.log", expected=1)

    # A role is enforced by the signed topic grant, not by the command-line name
    run(["./build/server", TRUSTED / "client1"], "client-as-server.log", expected=1)
    run(["./build/test_validation", "--probe", TRUSTED / "server"],
        "server-as-client.log", expected=1)

    with running(["./build/server", TRUSTED / "server"], "real-server.log") as server:
        # The probe retries discovery; no fixed startup sleep is needed here
        run(["./build/test_validation", "--probe", TRUSTED / "client1"], "trusted.log")
        run(["./build/test_validation", "--probe", FOREIGN / "client1"],
            "foreign-ca.log", expected=2)
        run(["./build/test_validation", "--plain"], "plaintext-participant.log")
        require(server.poll() is None, "Server exited during admission tests")
    print("PASS security checks against the real game server", flush=True)


