import os
import socket
import subprocess
import time
from collections import namedtuple

import pytest

from mock_hook import MockHook

HERE = os.path.dirname(os.path.abspath(__file__))
GKSDCP_BIN = os.environ.get("GKSDCP_BIN") or os.path.join(HERE, "..", "..", "gksdcp", "build", "gksdcp-host")

Server = namedtuple("Server", "ws_port udp_port web_port files usb var pid proc")


def _free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _wait_listening(proc, port, timeout=5.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if proc.poll() is not None:
            raise RuntimeError("gksdcp exited early with status %d" % proc.returncode)
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("gksdcp did not start listening on %d" % port)


@pytest.fixture
def mock_hook(tmp_path):
    # sockets must live under tmp_path (/tmp in the container), never on the NTFS bind mount
    hook = MockHook(tmp_path / "hook.sock")
    yield hook
    hook.close()


@pytest.fixture
def gksdcp_start(tmp_path, mock_hook):
    """Factory: gksdcp_start(extra=(), allow_tmpfs=True) -> Server. Dirs and sockets live under tmp_path (/tmp in
    the container). gksdcp talks to mock_hook. allow_tmpfs=False leaves out -T (the storage check is live)."""
    procs = []

    def start(extra=(), allow_tmpfs=True):
        files, usb, var = tmp_path / "files", tmp_path / "usb", tmp_path / "var"
        for d in (files, usb, var):
            d.mkdir(exist_ok=True)
        ws, udp, web = _free_port(), _free_port(), _free_port()
        cmd = [GKSDCP_BIN, "-p", str(ws), "-u", str(udp), "-w", str(web),
               "-d", str(files), "-U", str(usb), "-v", str(var),
               "-s", mock_hook.sock_path, "-m", "0000aabbccddeeff"] + (["-T"] if allow_tmpfs else []) + list(extra)
        proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL)
        procs.append(proc)
        _wait_listening(proc, ws)
        return Server(ws, udp, web, files, usb, var, proc.pid, proc)

    yield start
    for p in procs:
        if p.poll() is None:
            p.terminate()
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
