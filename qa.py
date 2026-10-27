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


