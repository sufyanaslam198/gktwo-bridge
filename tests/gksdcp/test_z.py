"""Z movement extension commands: Cmd 4096 Z_HOME, 4097 Z_MOVE {"Mm"}, 4098 Z_TOP, 4099 Z_STOP."""
import time

import pytest

from sdcp_client import ws_connect
from test_print import _ack, _hook_cmds, _notice, _start


def _z(mock_hook):
    return [r for r in mock_hook.received if str(r.get("cmd", "")).startswith("z_")]


def test_each_command_forwards_exact_line(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 4096)["Ack"] == 0
    assert _ack(ws, 4097, {"Mm": -10})["Ack"] == 0
    assert _ack(ws, 4098)["Ack"] == 0
    assert _ack(ws, 4099)["Ack"] == 0
    assert _z(mock_hook) == [{"cmd": "z_home"}, {"cmd": "z_move", "mm": -10}, {"cmd": "z_top"}, {"cmd": "z_stop"}]
    ws.close()


@pytest.mark.parametrize("mm", [1, 10, 100, -1, -10, -100])
def test_move_accepts_the_six_steps(gksdcp_start, mock_hook, mm):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 4097, {"Mm": mm})["Ack"] == 0
    assert _z(mock_hook) == [{"cmd": "z_move", "mm": mm}]
    ws.close()


@pytest.mark.parametrize("data", [{"Mm": 5}, {"Mm": 1.5}, {"Mm": "10"}, {"Mm": 0}, {"Mm": 1e1}, {"Mm": None}, {}, None])
def test_move_bad_mm_refused_without_hook_call(gksdcp_start, mock_hook, data):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 4097, data)["Ack"] == 1
    assert _notice(ws) == "bad request"
    assert _z(mock_hook) == []
    ws.close()


def test_hook_error_gives_ack1_and_notice(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    mock_hook.errors["z_move"] = "home Z first"
    assert _ack(ws, 4097, {"Mm": 10})["Ack"] == 1
    assert _notice(ws) == "home Z first"
    ws.close()


def test_status_shows_z(gksdcp_start, mock_hook):
    mock_hook.state["z"] = {"pos": 12.5, "homed": True, "moving": False}
    srv, ws = _start(gksdcp_start)
    time.sleep(0.6)                       # the 250 ms poll has picked the mock state up
    assert _ack(ws, 0)["Ack"] == 0
    st = ws.wait_topic("status", timeout=3.0)["Status"]
    assert st["GKBridge"]["Z"] == {"Pos": 12.5, "Homed": True, "Moving": False}
    ws.close()


def test_accepted_command_pushes_status(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 4096)["Ack"] == 0
    ws.msgs.clear()
    mock_hook.state["z"] = {"pos": 0.0, "homed": True, "moving": False}
    assert _ack(ws, 4098)["Ack"] == 0
    assert ws.wait_topic("status", timeout=2.0)["Status"]["GKBridge"]["Z"] is not None
    ws.close()


def test_hook_hang_replies_within_2_5s(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    mock_hook.mode = "hang"
    t0 = time.monotonic()
    assert _ack(ws, 4097, {"Mm": 1}, timeout=2.5)["Ack"] == 1
    assert time.monotonic() - t0 < 2.5
    assert _notice(ws) == "printer not responding"
    ws.close()


def test_busy_after_start_refuses_z_but_not_stop(gksdcp_start, mock_hook):
    srv, ws = _start(gksdcp_start)
    assert _ack(ws, 128, {"Filename": "/local/a.ctb"})["Ack"] == 0   # a start was just accepted: pending
    mock_hook.received.clear()
    for cmd, data in ((4096, None), (4097, {"Mm": 1}), (4098, None)):
        assert _ack(ws, cmd, data)["Ack"] == 1
        assert _notice(ws) == "printer busy"
    assert _z(mock_hook) == []
    assert _ack(ws, 4099)["Ack"] == 0
    assert _z(mock_hook) == [{"cmd": "z_stop"}]
    ws.close()
