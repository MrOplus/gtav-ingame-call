"""End-to-end test of texts and calls in both directions.

Starts its own plugin harness (ports 18765/18766) and its own Studio instance (18770/18771)
with a temporary settings file, so a Studio you are running for real is not touched.

    python tests/studio/e2e_twoway.py
"""
import asyncio
import fractions
import json
import math
import os
import pathlib
import queue
import socket
import subprocess
import sys
import tempfile
import threading
import time

import aioice.ice
import av
import numpy as np
import websockets
from aiortc import MediaStreamTrack, RTCPeerConnection, RTCSessionDescription

ROOT = pathlib.Path(__file__).resolve().parents[2]
HARNESS = ROOT / "build" / "harness"
STUDIO = ROOT / "studio"
PY = STUDIO / ".venv" / "Scripts" / "python.exe"
SIGNAL = "ws://127.0.0.1:18771/signal"
CODE = "twowaytest"
failures = []
HARNESS_INI = """[Server]
Host=127.0.0.1
HttpPort=18765
WsPort=18766
Token=s3cret
[Call]
RingTimeoutSec=25
[Audio]
Volume=0.0
"""



def check(cond, what):
    print(("  PASS " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)


def primary_only():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(("1.1.1.1", 53))
    ip = s.getsockname()[0]
    s.close()
    aioice.ice.get_host_addresses = lambda use_ipv4, use_ipv6: [ip]


class Harness:
    """The plugin in the harness; auto-answers incoming calls, records native output."""

    def __init__(self):
        self.p = subprocess.Popen([str(HARNESS / "harness.exe")], cwd=HARNESS, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)
        self.lines: queue.Queue = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            self.lines.put(line.rstrip())
            if "PLAY_PED_RINGTONE 'Remote_Ring'" in line:  # incoming call: answer after 1 s
                threading.Timer(1.0, lambda: self.send("y")).start()

    def send(self, cmd):
        self.p.stdin.write(cmd + "\n")
        self.p.stdin.flush()

    def drain(self):
        out = []
        while not self.lines.empty():
            out.append(self.lines.get())
        return out

    async def expect(self, needle, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self.lines.get_nowait()
                if needle in line:
                    return line
            except queue.Empty:
                await asyncio.sleep(0.05)
        return None

    def dial_first_contact(self):
        self.send("k 76")  # F7: contacts menu
        time.sleep(0.3)
        self.send("k 0D")  # Enter: call the selected contact


class Sine(MediaStreamTrack):
    kind = "audio"

    def __init__(self):
        super().__init__()
        self.pts, self.t0 = 0, None

    async def recv(self):
        self.t0 = self.t0 or time.monotonic()
        wait = self.t0 + self.pts / 48000 - time.monotonic()
        if wait > 0:
            await asyncio.sleep(wait)
        t = (np.arange(960) + self.pts) / 48000
        f = av.AudioFrame.from_ndarray((np.sin(2 * math.pi * 440 * t) * 8000).astype(np.int16).reshape(1, -1),
                                       format="s16", layout="mono")
        f.sample_rate, f.pts, f.time_base = 48000, self.pts, fractions.Fraction(1, 48000)
        self.pts += 960
        return f


async def recv_type(ws, *types, timeout=15):
    deadline = time.monotonic() + timeout
    while True:
        m = json.loads(await asyncio.wait_for(ws.recv(), max(0.1, deadline - time.monotonic())))
        if m["type"] in types:
            return m


async def friend(name):
    ws = await websockets.connect(SIGNAL)
    await ws.send(json.dumps({"type": "hello", "code": CODE, "name": name}))
    return ws, json.loads(await ws.recv())


async def make_offer():
    pc = RTCPeerConnection()
    pc.addTrack(Sine())
    got = {"frames": 0}

    @pc.on("track")
    def on_track(track):
        async def drain():
            while True:
                try:
                    await track.recv()
                    got["frames"] += 1
                except Exception:
                    return
        asyncio.ensure_future(drain())

    await pc.setLocalDescription(await pc.createOffer())
    return pc, got


async def main(h: Harness):
    ws, m = await friend("Ali")
    check(m["type"] == "welcome", "friend connected")
    await asyncio.sleep(0.5)

    print("text message from a friend")
    await ws.send(json.dumps({"type": "sms", "text": "Where are you? Meet at the pier.", "name": "Ali"}))
    r = await recv_type(ws, "sms_result")
    check(r["ok"], f"text delivered ({r})")
    check(await h.expect("FEED sender='Ali' subject='Message'") is not None, "text shown in-game from Ali")
    await ws.send(json.dumps({"type": "sms", "text": "spam", "name": "Ali"}))
    r = await recv_type(ws, "sms_result")
    check(not r["ok"] and r["reason"] == "slow_down", "rapid second text is throttled")

    print("player calls friend from the contacts menu, friend accepts")
    h.drain()
    h.dial_first_contact()
    inc = await recv_type(ws, "incoming")
    check(inc.get("from") == "Tester", f"friend's page rings with host name ({inc})")
    check(await h.expect("PLAY_PED_RINGTONE 'Dial_and_Remote_Ring'") is not None, "player hears ringback in-game")
    pc, got = await make_offer()
    await ws.send(json.dumps({"type": "accept", "sdp": pc.localDescription.sdp, "name": "Ali"}))
    ans = await recv_type(ws, "answer")
    await pc.setRemoteDescription(RTCSessionDescription(sdp=ans["sdp"], type="answer"))
    c = await recv_type(ws, "connected")
    check(c is not None, "call connected after friend accepted")
    await asyncio.sleep(2.5)
    check(pc.connectionState == "connected" and got["frames"] > 50, f"audio from the game reaches the friend ({got['frames']} frames)")
    await ws.send(json.dumps({"type": "hangup"}))
    e = await recv_type(ws, "ended")
    check(e["reason"] == "hung_up", f"friend hung up ({e['reason']})")
    await pc.close()
    await asyncio.sleep(1)

    print("player calls friend, friend declines")
    h.drain()
    h.dial_first_contact()
    await recv_type(ws, "incoming")
    await ws.send(json.dumps({"type": "decline"}))
    e = await recv_type(ws, "ended")
    check(e["reason"] == "declined", f"ended as declined ({e['reason']})")
    check(await h.expect("subject='Call failed'") is not None, "game shows 'Call failed' notification")
    await asyncio.sleep(1)

    print("player calls friend, then cancels before they answer")
    h.drain()
    h.dial_first_contact()
    await recv_type(ws, "incoming")
    await asyncio.sleep(0.5)
    h.send("n")
    e = await recv_type(ws, "ended")
    check(e["reason"] == "cancelled", f"friend sees the call was cancelled ({e['reason']})")
    await asyncio.sleep(1)

    print("friend calls player (regression)")
    pc, got = await make_offer()
    await ws.send(json.dumps({"type": "call", "sdp": pc.localDescription.sdp, "name": "Ali"}))
    ans = await recv_type(ws, "answer")
    await pc.setRemoteDescription(RTCSessionDescription(sdp=ans["sdp"], type="answer"))
    check((await recv_type(ws, "connected")) is not None, "incoming call still connects")
    await ws.send(json.dumps({"type": "hangup"}))
    await recv_type(ws, "ended")
    await pc.close()

    print("game phone: friend in Contacts app, call from the phone's own UI")
    await asyncio.sleep(1)
    h.drain()
    h.send("phone contacts")  # phone up with the Contacts app open
    check(await h.expect("[scaleform] SET_DATA_SLOT 2 50 0 'Ali' CELL_999 'CHAR_DEFAULT'") is not None,
          "friend injected into the phone's Contacts list (slot 50)")
    h.send("phone select 3")  # one of the game's own contacts
    await asyncio.sleep(0.6)
    check(not any("TERMINATE appcontacts" in l for l in h.drain()), "selecting a game contact is left to the game")
    h.send("phone select 50")
    inc = await recv_type(ws, "incoming")
    check(inc is not None, "selecting the friend in the phone rings their page")
    lines = h.drain() + [await h.expect("Dial_and_Remote_Ring") or ""]
    check(any("TERMINATE appcontacts" in l for l in lines), "game's contacts app is stopped for our call")
    check(any("SET_DATA_SLOT 4 0 3 'Ali' 'CHAR_DEFAULT' CELL_211" in l for l in lines), "phone shows its call screen: DIALING")
    check(any("Dial_and_Remote_Ring" in l for l in lines), "phone ringback sound plays")
    pc, got = await make_offer()
    await ws.send(json.dumps({"type": "accept", "sdp": pc.localDescription.sdp, "name": "Ali"}))
    ans = await recv_type(ws, "answer")
    await pc.setRemoteDescription(RTCSessionDescription(sdp=ans["sdp"], type="answer"))
    await recv_type(ws, "connected")
    check(await h.expect("SET_DATA_SLOT 4 0 3 'Ali' 'CHAR_DEFAULT' CELL_219") is not None, "phone call screen switches to CONNECTED")
    await asyncio.sleep(1)
    h.send("phone cancel")  # the phone's own back/hang-up button
    e = await recv_type(ws, "ended")
    check(e["reason"] == "host_hung_up", f"hanging up on the phone ends the call ({e['reason']})")
    check(await h.expect("SHUTDOWN_MOVIE") is not None and await h.expect("START_NEW_SCRIPT cellphone_controller") is not None,
          "phone is put away and its scripts restarted")
    await pc.close()
    h.send("phone down")
    await asyncio.sleep(1)

    print("friend leaves -> no longer callable")
    await ws.close()
    await asyncio.sleep(1)
    h.drain()
    h.dial_first_contact()
    check(await h.expect("Dial_and_Remote_Ring", timeout=2) is None, "nobody to call once the friend left")


if __name__ == "__main__":
    primary_only()
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="phonelink-test-"))
    (tmp / "settings.json").write_text(json.dumps({
        "plugin": {"host": "127.0.0.1", "http_port": 18765, "ws_port": 18766, "token": "s3cret"},
        "calls": {"display_name": "Tester", "invite_code": CODE}}))
    (HARNESS / "PhoneLink.ini").write_text(HARNESS_INI)
    h = Harness()
    studio = subprocess.Popen([str(PY), "app.py", "--no-browser", "--port", "18770", "--public-port", "18771"],
                              cwd=STUDIO, env={**os.environ, "PHONELINK_STUDIO_SETTINGS": str(tmp / "settings.json")},
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(4)
    try:
        asyncio.run(main(h))
    finally:
        studio.kill()
        h.send("q")
        time.sleep(0.5)
        h.p.kill()
    print(f"\n{len(failures)} failure(s)")
    for f in failures:
        print("  -", f)
    if failures:
        print("studio log:", tmp / "studio.log")
    sys.exit(1 if failures else 0)
