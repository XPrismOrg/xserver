# SPDX-License-Identifier: MIT
#
# Servers started at the same time with -displayfd each scan for a free
# display. A socket that another server has bound but is not listening on
# yet refuses connections and looked stale: the second server unlinked it
# and took the display too, and the first one's clients found no socket
# (os/Xtranssock.c, SocketUNIXCreateListener). Every server's socket file
# must exist for as long as it runs.
#
# The servers here are started directly, not through XServerProcess,
# whose lock serializes the starts of the test suite's own servers.

import os
import resource
import select
import subprocess
import threading
import time

import pytest

THREADS, SERVERS = 3, 60


def _start(xvfb):
    r, w = os.pipe()
    p = subprocess.Popen([xvfb, "-displayfd", str(w), "-nolisten", "tcp",
                          "-screen", "0", "64x64x24"],
                         pass_fds=[w], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    os.close(w)
    data = b""
    end = time.monotonic() + 30
    while not data.endswith(b"\n"):
        # a server that neither starts nor exits is reported, not waited for
        if not select.select([r], [], [], max(0, end - time.monotonic()))[0]:
            p.kill()
            p.wait()
            os.close(r)
            return p, "silent"
        chunk = os.read(r, 16)
        if not chunk:
            break
        data += chunk
    os.close(r)
    return p, int(data) if data.strip() else None


def test_concurrent_starts_keep_their_sockets():
    xvfb = os.environ.get("XVFB_PATH")
    if not xvfb:
        pytest.skip("XVFB_PATH not set")
    lost, failed, silent, stuck = [], [], [], []

    def loop():
        for _ in range(SERVERS):
            p, display = _start(xvfb)
            if display == "silent":
                silent.append(p.pid)
                continue
            if display is None:
                failed.append(p.wait())
                continue
            for _ in range(5):
                time.sleep(0.01)
                if not os.path.exists(f"/tmp/.X11-unix/X{display}"):
                    lost.append(display)
                    break
            p.terminate()
            try:
                p.wait(timeout=30)
            except subprocess.TimeoutExpired:
                stuck.append(display)
                p.kill()
                p.wait()

    threads = [threading.Thread(target=loop) for _ in range(THREADS)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not silent, f"servers that never reported a display: {len(silent)}"
    assert not stuck, f"servers that ignored SIGTERM: displays {stuck}"
    assert not failed, f"servers that did not start: {failed}"
    assert not lost, f"running servers lost their sockets: displays {lost}"
