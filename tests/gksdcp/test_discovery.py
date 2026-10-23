import errno
import ipaddress
import json
import os
import socket

import pytest

from sdcp_client import discover

with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "VERSION")) as f:
    VERSION = f.read().strip()

KEYS = {"Name", "MachineName", "BrandName", "MainboardIP", "MainboardID", "ProtocolVersion", "FirmwareVersion"}


def test_unicast_reply_fields(gksdcp_start):
    srv = gksdcp_start()
    r = discover(srv.udp_port)
    assert len(r["Id"]) == 32 and r["Id"] == r["Id"].lower()
    int(r["Id"], 16)
    d = r["Data"]
    assert set(d) == KEYS
    assert d["ProtocolVersion"] == "V3.0.0"
    assert d["MainboardID"] == "0000aabbccddeeff"
    assert d["MainboardIP"] == "127.0.0.1"
    assert d["MachineName"] == "GKtwo"
    assert d["Name"] == "GKtwo"
    assert d["BrandName"] == "Uniformation"
    assert d["FirmwareVersion"].endswith("-bridge-" + VERSION)
    assert "Proxy" not in json.dumps(r)


def test_reply_goes_to_sender_port(gksdcp_start):
    srv = gksdcp_start()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    s.settimeout(2)
    try:
        s.sendto(b"M99999\r\n", ("127.0.0.1", srv.udp_port))
        data, frm = s.recvfrom(4096)
    finally:
        s.close()
    assert frm == ("127.0.0.1", srv.udp_port)
    assert json.loads(data)["Data"]["MainboardID"] == "0000aabbccddeeff"


@pytest.mark.parametrize("payload", [b"hello", b"M99999x", b"m99999", b"", b"M99999 \r\n", b" M99999"])
def test_other_payload_ignored(gksdcp_start, payload):
    srv = gksdcp_start()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.4)
    try:
        s.sendto(payload, ("127.0.0.1", srv.udp_port))
        with pytest.raises(socket.timeout):
            s.recvfrom(4096)
        s.sendto(b"M99999", ("127.0.0.1", srv.udp_port))     # still alive afterwards
        s.settimeout(2)
        assert json.loads(s.recvfrom(4096)[0])["Data"]["MainboardID"]
    finally:
        s.close()


def test_broadcast_request_reports_interface_address(gksdcp_start):
    srv = gksdcp_start()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(2)
    try:
        try:
            s.sendto(b"M99999", ("255.255.255.255", srv.udp_port))
        except OSError as e:
            if e.errno not in (errno.ENETUNREACH, errno.EACCES, errno.EPERM):
                raise
            pytest.skip("no broadcast route in this environment")
        data, _ = s.recvfrom(4096)       # a timeout here is a failure
    finally:
        s.close()
    ip = ipaddress.IPv4Address(json.loads(data)["Data"]["MainboardIP"])
    assert str(ip) not in ("255.255.255.255", "0.0.0.0")


def test_rename_shows_in_discovery(gksdcp_start):
    from sdcp_client import ws_connect
    srv = gksdcp_start()
    ws = ws_connect(srv.ws_port)
    rid = ws.request(192, {"Name": "Resin Bot"})
    ws.wait_response(rid)
    ws.close()
    assert discover(srv.udp_port)["Data"]["Name"] == "Resin Bot"
