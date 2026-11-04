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


ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build" / "qa"
TRUSTED = WORK / "trusted"
FOREIGN = WORK / "foreign"

# These processes need to discover one another, not another group's game on the
# LAN; explicit unicast peers also make the tests usable inside a network namespace
LOCAL_DDS = """<CycloneDDS><Domain Id="42"><General><Interfaces>
<NetworkInterface name="lo" multicast="false"/></Interfaces>
<AllowMulticast>false</AllowMulticast></General><Discovery>
<ParticipantIndex>auto</ParticipantIndex><MaxAutoParticipantIndex>63</MaxAutoParticipantIndex><Peers><Peer Address="127.0.0.1"/></Peers>
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
    # Reuse only this harness's credentials; scripts/security.sh refuses replacement, so
    # rerunning QA cannot silently invalidate the real team's certificate set
    for directory in (TRUSTED, FOREIGN):
        if not directory.exists():
            run(["bash", "scripts/security.sh", directory], f"create-{directory.name}.log")
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
    run(["./build/test_game"], "game-tests.log")
    # Exercise generation on a fresh path as well as reusing earlier QA keys
    # A path with spaces catches shell-quoting mistakes before a teammate hits one
    with tempfile.TemporaryDirectory(prefix="issuance-", dir=WORK) as temporary:
        directory = Path(temporary) / "keys with spaces"
        run(["bash", "scripts/security.sh", directory], "fresh-issuance.log")
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
            run(["./build/test_validation", "--probe", directory / "client1", "--create"],
                "fresh-client.log")
    run(["./build/test_validation", TRUSTED], "integration.log")
    run(["./build/server", WORK / "missing"], "missing-server.log", expected=1)
    run(["./build/test_validation", "--probe", WORK / "missing"],
        "missing-client.log", expected=1)

    # An existing output path must fail without changing any private key bytes
    original = (TRUSTED / "authority" / "ca.key").read_bytes()
    run(["bash", "scripts/security.sh", TRUSTED], "no-overwrite.log", expected=1)
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
        run(["./build/test_validation", "--probe", TRUSTED / "client1", "--create"], "trusted.log")
        run(["./build/test_validation", "--probe", FOREIGN / "client1"],
            "foreign-ca.log", expected=2)
        run(["./build/test_validation", "--plain"], "plaintext-participant.log")
        run(["./build/test_validation", "--rooms", TRUSTED], "room-security.log")
        # Start 2 real clients together so the existing game starts a round;
        # both must receive movement, coin state and the final server result
        with contextlib.ExitStack() as clients:
            participants = [clients.enter_context(running(
                ["./build/test_validation", "--game", TRUSTED / f"client{i}",
                 *(["--create"] if i == 1 else [])],
                f"game-client-{i}.log")) for i in (1, 2)]
            for client in participants:
                require(client.wait(timeout=150) == 0, "Actual game transport check failed")
        print("PASS full-round transport with 2 actual game clients", flush=True)
        require(server.poll() is None, "Server exited during admission tests")
    print("PASS security checks against the real game server", flush=True)


def cpu_sample(process):
    # /proc reports the process's accumulated user and kernel CPU ticks, including
    # its threads; 1 fully occupied logical core therefore corresponds to 100%
    fields = Path(f"/proc/{process.pid}/stat").read_text().rsplit(")", 1)[1].split()
    cpu = (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
    status = Path(f"/proc/{process.pid}/status").read_text().splitlines()
    peak = int(next(line for line in status if line.startswith("VmHWM:")).split()[1])
    return cpu, peak


def profile():
    # Remove only the DDS polling sleep in the control build; the game must keep
    # its fixed tick interval so the comparison does not change game speed
    source = (ROOT / "src" / "server.c").read_text()
    sleep = "dds_sleepfor(DDS_MSECS(3));"
    require(source.count(sleep) == 1, "Update the polling experiment for the changed worker")
    busy_source = WORK / "server-busy.c"
    busy_source.write_text(source.replace(sleep, "(void)0; /* QA busy-poll control */"))
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "CycloneDDS"], text=True))
    busy = WORK / "server-busy"
    run(["gcc", "-std=c11", "-D_DEFAULT_SOURCE", "-O2", "-g", "-Wall", "-Wextra",
         "-Wpedantic", "-Iinclude", "-Ivendor", "-Ibuild", busy_source, "src/game.c", "src/network.c", "build/messages.c",
         "-o", busy, *flags, "-pthread", "-lcrypto"], "profile-build.log")

    results = {}
    for label, binary in (("busy_poll_control", busy), ("bounded_poll", "./build/server")):
        samples = []
        for trial in range(3):
            with running([binary, TRUSTED / "server"], f"profile-{label}-{trial}.log") as process:
                time.sleep(0.5)
                before, _ = cpu_sample(process)
                started = time.monotonic()
                time.sleep(3)
                after, peak = cpu_sample(process)
                elapsed = time.monotonic() - started
                samples.append({"seconds": round(elapsed, 3),
                                "cpu_percent_one_core": round(100 * (after - before) / elapsed, 2),
                                "peak_rss_kib": peak})
        results[label] = samples

    # Repeat at the 5-player capacity; each headless client sends movement and
    # receives snapshots; graphics cost is outside this networking benchmark
    with contextlib.ExitStack() as stack:
        process = stack.enter_context(running(
            ["./build/server", TRUSTED / "server"], "profile-loaded-server.log"))
        clients = [stack.enter_context(running(
            ["./build/test_validation", "--link", TRUSTED / f"client{i}",
             *(["--create"] if i == 1 else [])],
            f"profile-client-{i}.log")) for i in range(1, 6)]
        time.sleep(2)
        before, _ = cpu_sample(process)
        started = time.monotonic()
        time.sleep(3)
        after, peak = cpu_sample(process)
        elapsed = time.monotonic() - started
        results["five_clients"] = {"seconds": round(elapsed, 3),
                                  "cpu_percent_one_core": round(100 * (after - before) / elapsed, 2),
                                  "peak_rss_kib": peak}
        for client in clients:
            require(client.wait(timeout=8) == 0, "Loaded benchmark client failed")

    (WORK / "profile.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2), flush=True)

    # Callgrind identifies where instructions are spent; its instruction counts
    # are not CPU time; keep them separate from the normal-build measurements
    require(shutil.which("valgrind"), "Install valgrind to collect the profiler evidence")
    with running(["valgrind", "--tool=callgrind", f"--callgrind-out-file={WORK / 'callgrind.out'}",
                  "./build/server", TRUSTED / "server"], "callgrind.log") as process:
        time.sleep(5)
        process.send_signal(signal.SIGINT)
        require(process.wait(timeout=10) in (0, -signal.SIGINT), "Profiled server failed to stop")
    run(["callgrind_annotate", "--inclusive=yes", "--threshold=95", WORK / "callgrind.out"],
        "callgrind-report.txt")


def rtps_counts(data):
    # Walk RTPS submessages by their declared lengths; searching raw ciphertext
    # for byte patterns could mistake encrypted bytes for a plaintext message
    secure = plain = 0
    if not data.startswith(b"RTPS") or len(data) < 20:
        return secure, plain
    position = 20
    while position + 4 <= len(data):
        kind, flags = data[position:position + 2]
        size = int.from_bytes(data[position + 2:position + 4],
                              "little" if flags & 1 else "big")
        end = position + 4 + size if size else len(data)
        require(end <= len(data), "Truncated RTPS submessage in capture")
        if kind in (0x30, 0x31, 0x33):
            secure += 1

        # DATA and DATA_FRAG place the writer entity kind at byte 15; builtin
        # discovery/authentication writers have the high bits set; application
        # writers do not; bootstrap traffic is allowed, plaintext game data is not
        if kind in (0x15, 0x16) and end - position >= 16:
            if data[position + 15] & 0xC0 == 0:
                plain += 1
        if size == 0:
            break
        position = end
    return secure, plain


def parser_check():
    # A small synthetic check keeps the packet-evidence parser honest, including
    # fragmented DATA and the distinction between builtin and application writers
    header = b"RTPS" + bytes(16)
    user = bytes((0x15, 1, 12, 0)) + bytes(12)
    builtin = bytearray(user)
    builtin[15] = 0xC2
    fragment = bytes((0x16,)) + user[1:]
    require(rtps_counts(header + user) == (0, 1), "DATA parser check failed")
    require(rtps_counts(header + builtin) == (0, 0), "Builtin parser check failed")
    require(rtps_counts(header + fragment) == (0, 1), "DATA_FRAG parser check failed")
    require(rtps_counts(header + bytes((0x33, 1, 0, 0))) == (1, 0), "Secure parser check failed")


def namespace_only():
    # make invokes these modes through unshare; refuse the original namespace
    # even when root; accidentally impairing the user's LAN is not acceptable
    require(os.geteuid() == 0, "Use make wire or make loss for network privileges")
    require(os.readlink("/proc/self/ns/net") != os.readlink("/proc/1/ns/net"),
            "Refusing network changes outside a separate network namespace")
    subprocess.run(["ip", "link", "set", "lo", "up"], check=True)


def wire():
    namespace_only()
    parser_check()
    packets = secure = plain = 0

    # The namespace uses IPv4 loopback only; linux exposes a 14-byte link header
    # on this AF_PACKET socket, followed by IPv4 and UDP; no unrelated LAN traffic
    # is captured; packets are counted rather than saving other processes' data
    with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as capture:
        capture.bind(("lo", 0))
        capture.settimeout(0.1)
        with running(["./build/test_validation", TRUSTED], "wire-integration.log") as process:
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                try:
                    frame = capture.recv(65535)
                except socket.timeout:
                    if process.poll() is not None:
                        break
                    continue
                if len(frame) < 42 or frame[12:14] != b"\x08\x00" or frame[23] != 17:
                    continue
                udp = 14 + (frame[14] & 15) * 4
                data = frame[udp + 8:]
                if not data.startswith(b"RTPS"):
                    continue
                packets += 1
                protected, exposed = rtps_counts(data)
                secure += protected > 0
                plain += exposed
            require(process.wait(timeout=3) == 0, "Integration tests failed during capture")
    result = {"rtps_packets_observed": packets, "packets_with_security_submessages": secure,
              "plaintext_user_DATA_or_DATA_FRAG": plain,
              "note": "Loopback may expose each packet twice; counts are observations."}
    (WORK / "wire.json").write_text(json.dumps(result, indent=2) + "\n")
    require(secure > 100 and plain == 0, "No sufficient encrypted traffic, or plaintext game data found")
    print(json.dumps(result, indent=2), flush=True)


def loss():
    namespace_only()
    # Apply both cases to an isolated loopback device; each uses the actual game
    # server and a client that must stay admitted and receive at least 30 states
    for label, delay, percent in (("reference", "1ms", "0%"), ("impaired", "15ms", "10%")):
        subprocess.run(["tc", "qdisc", "replace", "dev", "lo", "root", "netem",
                        "delay", delay, "1ms", "loss", percent], check=True)
        try:
            with running(["./build/server", TRUSTED / "server"], f"loss-{label}-server.log"):
                output = run(["./build/test_validation", "--link", TRUSTED / "client1", "--create"],
                             f"loss-{label}-client.log", timeout=15)
                print(output, end="", flush=True)
                run(["tc", "-s", "qdisc", "show", "dev", "lo"], f"loss-{label}-queue.log")
        finally:
            subprocess.run(["tc", "qdisc", "del", "dev", "lo", "root"], check=True)


def main():
    require(len(sys.argv) == 2 and sys.argv[1] in ("check", "sanitize", "profile", "wire", "loss"),
            "Usage: python3 tests/qa.py check|sanitize|profile|wire|loss")
    os.chdir(ROOT)
    prepare()
    mode = sys.argv[1]
    if mode == "check":
        checks()
    elif mode == "sanitize":
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        run(["./build/test_game_sanitized"], "game-sanitizers.log", env=env)
        run(["./build/test_sanitized", TRUSTED], "sanitizers.log", env=env)
        # Also exercise the actual threaded server adapter; the protocol fixture
        # alone cannot find memory errors in its main/worker handoff or shutdown
        with running(["./build/server_sanitized", TRUSTED / "server"],
                     "sanitized-server.log", env) as server:
            run(["./build/test_sanitized", "--rooms", TRUSTED],
                "sanitized-rooms.log", env=env)
            run(["./build/test_sanitized", "--link", TRUSTED / "client1", "--create"],
                "sanitized-link.log", env=env)
            server.send_signal(signal.SIGINT)
            require(server.wait(timeout=5) in (0, -signal.SIGINT), "Sanitized game server failed on shutdown")
    elif mode == "profile":
        profile()
    elif mode == "wire":
        wire()
    else:
        loss()
    print(f"Finished {mode}; evidence: {WORK}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
