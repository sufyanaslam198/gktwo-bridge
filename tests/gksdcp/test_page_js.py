"""Behavioural checks of the page script under node with a fake DOM and WebSocket (skipped without node)."""
import json
import os
import re
import shutil
import subprocess

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
INDEX = os.path.join(HERE, "..", "..", "gksdcp", "www", "index.html")

pytestmark = pytest.mark.skipif(shutil.which("node") is None, reason="node not installed")

MB = 1048576


def run(tmp_path, scenario):
    html = open(INDEX, encoding="utf-8").read()
    js = tmp_path / "page.js"
    js.write_text(re.search(r"<script>(.*)</script>", html, re.S).group(1), encoding="utf-8")
    out = subprocess.run(["node", os.path.join(HERE, "page_harness.js"), str(js), scenario],
                         capture_output=True, text=True, timeout=20)
    assert out.returncode == 0, out.stderr
    return json.loads(out.stdout.strip().splitlines()[-1])


def test_connect_sends_1_then_0(tmp_path):
    r = run(tmp_path, "connect")
    assert r["url"] == "ws://printer.local:3030/websocket"
    assert r["cmds"][:2] == [1, 0]
    assert 258 in r["cmds"][2:]                      # then the file list
    assert r["rids"] is True                         # a fresh RequestID each time
    assert r["topic"].startswith("sdcp/request/")
    assert r["keepalive_ms"] and all(ms <= 60000 for ms in r["keepalive_ms"])
    assert r["keepalive_cmds"] == [0]                # the server's idle timer counts inbound frames only


def test_status_printing_renders(tmp_path):
    r = run(tmp_path, "status_printing")
    assert r["state"] == "Printing"
    assert r["layer"] == "5 / 10"
    assert r["elapsed"] == "0:01:00" and r["remaining"] == "0:02:00"
    assert r["progress"] == 50
    assert r["file"] == "/local/a.ctb"
    assert r["ctl"] == [False, True, False]          # Pause, Stop
    assert r["print_del"] == [True, True]
    assert r["zpos"] == "Z: 12.35 mm"
    assert r["paused"] == "Paused"
    assert r["paused_ctl"] == [True, False, False]   # Resume, Stop
    assert r["msg"] == "careful <b>x</b>"            # text, not markup


def test_notice_after_nack_shown(tmp_path):
    r = run(tmp_path, "notice_after_nack")
    assert r["cmd"] == 129 and r["disabled_during"] is True      # no double submit
    assert r["before_timer"] == "printer busy"
    assert r["msg"] == "printer busy" and r["cls"] == "err"


def test_nack_without_notice_says_refused(tmp_path):
    r = run(tmp_path, "nack_without_notice")
    assert r["before_timer"] == ""
    assert r["msg"] == "Pause refused" and r["cls"] == "err"


def test_ack_ok_reports_sent(tmp_path):
    r = run(tmp_path, "ack_ok")
    assert r["msg"] == "Pause sent" and r["cls"] == ""


def test_print_button_disabled_for_non_ctb(tmp_path):
    r = run(tmp_path, "print_nonctb")                # a.ctb (Print, Delete), b.cbddlp (Print, Delete)
    assert r["buttons"] == [False, False, True, False]
    assert r["titles"][0] == "" and ".ctb" in r["titles"][2]
    assert r["start"] == {"Filename": "/local/a.ctb", "StartLayer": 0}
    assert r["delete"] == {"FileList": ["/local/a.ctb"]}


def test_usb_tab_follows_attributes(tmp_path):
    r = run(tmp_path, "usb_tab")
    assert r["usb_hidden"] == "none" and r["usb_shown"] != "none"
    assert "free" in r["free"]
    assert r["list_url"] == "/usb/" and r["rows"] == 1
    assert r["start"]["Filename"] == "/usb/u.ctb"
    assert r["usb_gone"] == "none" and r["list_url_after"] == "/local/"      # falls back to internal storage


def test_upload_chunks_and_offsets(tmp_path):
    r = run(tmp_path, "upload_chunks")
    f = r["fetches"]
    assert [x["offset"] for x in f] == ["0", str(MB), str(2 * MB)]
    assert [x["size"] for x in f] == [MB, MB, MB // 2]
    assert {x["total"] for x in f} == {str(2 * MB + MB // 2)}
    assert len({x["uuid"] for x in f}) == 1 and f[0]["uuid"]
    assert {x["check"] for x in f} == {"0"} and {x["name"] for x in f} == {"big.ctb"}
    assert f[0]["url"] == "http://printer.local:3030/uploadFile/upload" and f[0]["method"] == "POST"
    assert r["msg"] == "Uploaded big.ctb" and r["cls"] == ""


def test_upload_failure_shows_readable_message(tmp_path):
    r = run(tmp_path, "upload_fail")
    assert len(r["fetches"]) == 1                    # stops at the first refused chunk
    assert r["msg"] == "printer busy" and r["cls"] == "err"
    r = run(tmp_path, "upload_fail_code")
    assert r["msg"] == "upload failed (code -2)"


def test_upload_cancel_sends_cmd_255(tmp_path):
    r = run(tmp_path, "upload_cancel")
    assert r["cancel_enabled"] is True
    assert r["cancel"]["Uuid"] == r["fetches"][0]["uuid"] and r["cancel"]["FileName"] == "big.ctb"
    assert r["msg"] == "Upload cancelled"


def test_z_buttons_send_4097_with_mm(tmp_path):
    r = run(tmp_path, "z_buttons")
    assert r["enabled"] == [False, False, False]
    assert r["sent"] == [[4097, {"Mm": 10}], [4097, {"Mm": -1}], [4098, {}], [4096, {}], [4099, {}]]
    assert r["confirms"] == 2                        # TOP and Home ask; jogs and stop do not
    assert r["unhomed"] == [True, True, False]       # moves need a homed axis, Home does not
    assert r["hint"] == "Home Z first"
    assert r["busy"] == [True, True, True, True]     # printing


def test_reconnect_no_flicker(tmp_path):
    r = run(tmp_path, "reconnect_no_flicker")
    assert r["after_first_drop"] == "none"           # one lost connection is not "not reachable"
    assert r["state_cleared"] == "—"
    assert r["reconnect_timer"] is True and r["sockets"] == 2
    assert r["after_second_drop"] == "block"         # the retry failed too
    assert r["after_open"] == "none"


def test_notice_of_other_type_is_not_a_refusal_text(tmp_path):
    r = run(tmp_path, "notice_other_type")
    assert r["before_timer"] == "" and r["msg"] == "Pause refused"


def test_hook_down_uses_hookok(tmp_path):
    r = run(tmp_path, "hook_down")
    assert r["buttons"] == [True, True]              # Print, Delete
    assert r["z"] == "Z control unavailable"
    assert r["msg"] == "hook: not running"
    assert r["buttons_ok"] == [False, False]


def test_empty_file_rejected(tmp_path):
    r = run(tmp_path, "upload_empty")
    assert r["fetches"] == 0 and r["msg"] == "file is empty" and r["cls"] == "err"


def test_failure_after_accepted_start_shown(tmp_path):
    r = run(tmp_path, "start_fails_later")
    assert r["sent"] == "Start a.ctb sent"
    assert r["msg"] == "USB drive removed during copy" and r["cls"] == "err"


def test_notice_after_print_runs_not_shown(tmp_path):
    r = run(tmp_path, "start_then_prints")
    assert r["msg"] == "Start a.ctb sent" and r["cls"] == ""


def test_notice_after_start_ended_without_one_not_shown(tmp_path):
    r = run(tmp_path, "start_cancelled_elsewhere")
    assert r["msg"] == "Start a.ctb sent" and r["cls"] == ""
