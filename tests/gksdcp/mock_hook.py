"""Mock control-hook: unix-socket server speaking the gkhook control protocol."""
import json
import os
import socket
import threading


class MockHook:
    def __init__(self, sock_path):
        self.sock_path = str(sock_path)
        self.state = {"ok": True, "state": "idle", "file": None, "layer": None,
                      "total_layers": None, "progress": None,
                      "elapsed_s": None, "remaining_s": None,
                      "z": {"pos": None, "homed": False, "moving": False},
                      "phase": None, "screen": [7680, 4320]}
        self.received = []
        self._mode = "ok"
        self.delays = {}  # cmd -> seconds to wait before answering it
        self.errors = {}  # cmd -> error text: answer that command with {"ok":false,"error":...}
        self.raw = None  # if set, bytes sent verbatim as the reply to every request
        self.on_cmd = {}  # cmd -> fn(state): changes the state before the reply (as gkhook can, e.g. a start that fails at once)
        self._stop = threading.Event()
        self._srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._srv.bind(self.sock_path)
        self._srv.listen(64)
        self._thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._thread.start()

    @property
    def mode(self):
        return self._mode

    @mode.setter
    def mode(self, value):
        assert value in ("ok", "hang", "absent")
        self._mode = value
        if value == "absent" and os.path.exists(self.sock_path):
            os.unlink(self.sock_path)

    def _accept_loop(self):
        while not self._stop.is_set():
            try:
                conn, _ = self._srv.accept()
            except OSError:
                return
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    def _handle(self, conn):
        try:
            data = b""
            while not data.endswith(b"\n"):
                chunk = conn.recv(4096)
                if not chunk:
                    break
                data += chunk
            if self._mode == "hang":
                self._stop.wait(30)
                return
            try:
                req = json.loads(data.decode())
            except ValueError:
                conn.sendall(b'{"ok":false,"error":"bad json"}\n')
                return
            self.received.append(req)
            if req.get("cmd") in self.delays:
                self._stop.wait(self.delays[req["cmd"]])
            if self.raw is not None:
                conn.sendall(self.raw)
                return
            if req.get("cmd") in self.on_cmd:
                self.on_cmd[req["cmd"]](self.state)
            if req.get("cmd") in self.errors:
                reply = {"ok": False, "error": self.errors[req["cmd"]]}
            elif req.get("cmd") == "status":
                reply = self.state
            else:
                reply = {"ok": True}  # z_* too: no motion is simulated, tests set state["z"]
            conn.sendall(json.dumps(reply).encode() + b"\n")
        except (BrokenPipeError, ConnectionResetError):
            pass                   # the client gave up before the reply (timeout tests)
        finally:
            conn.close()

    def close(self):
        self._stop.set()
        self._srv.close()
