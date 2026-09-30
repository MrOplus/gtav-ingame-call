"""End-to-end test of PhoneLink.asi running in the out-of-game harness.

    python tests/harness/e2e.py [build/harness]
"""
import asyncio
import base64
import io
import json
import math
import pathlib
import struct
import subprocess
import sys
import urllib.error
import urllib.request
import wave

import websockets

HARNESS = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build/harness").resolve()
HTTP = "http://127.0.0.1:18765"
WS = "ws://127.0.0.1:18766"
TOKEN = "s3cret"

INI = f"""[Server]
Host=127.0.0.1
HttpPort=18765
WsPort=18766
Token={TOKEN}
[Call]
RingTimeoutSec=25
[Audio]
Volume=0.0
"""

failures = []


def check(cond, what):
    print(("  PASS " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def http(method, path, body=None, token=TOKEN):
    req = urllib.request.Request(HTTP + path, method=method,
                                 data=json.dumps(body).encode() if body is not None else None)
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def make_wav(seconds=1.0, rate=16000):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 440 * i / rate)))
                               for i in range(int(seconds * rate))))
    return buf.getvalue()


class Events:
    def __init__(self, ws):
        self.ws = ws
        self.queue = asyncio.Queue()
        self.binary_bytes = 0

    async def pump(self):
        async for msg in self.ws:
            if isinstance(msg, bytes):
                self.binary_bytes += len(msg)
            else:
                await self.queue.put(json.loads(msg))

    async def expect(self, event, timeout=15):
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while True:
            remaining = deadline - loop.time()
            if remaining <= 0:
                return None
            try:
                m = await asyncio.wait_for(self.queue.get(), remaining)
            except asyncio.TimeoutError:
                return None
            if m.get("event") == event:
                return m


async def main():
    (HARNESS / "PhoneLink.ini").write_text(INI)
    proc = subprocess.Popen([str(HARNESS / "harness.exe")], cwd=HARNESS, stdin=subprocess.PIPE, text=True)

    def key(k):
        proc.stdin.write(k + "\n")
        proc.stdin.flush()

    await asyncio.sleep(1.5)
    try:
        print("auth")
        check(http("GET", "/status", token=None)[0] == 401, "HTTP without token is rejected")
        try:
            async with websockets.connect(WS) as bad:
                await asyncio.wait_for(bad.recv(), 3)
                check(False, "WS without token is rejected")
        except Exception:
            check(True, "WS without token is rejected")

        async with websockets.connect(f"{WS}/?token={TOKEN}", max_size=None) as ws:
            ev = Events(ws)
            pump = asyncio.create_task(ev.pump())
            check((await ev.expect("hello")) is not None, "WS hello event")

            print("status")
            code, body = http("GET", "/status")
            check(code == 200 and body.get("in_game") is True and body.get("call_state") == "idle", f"status {body}")

            print("sms")
            code, body = http("POST", "/sms", {"from": "Lester", "subject": "Job", "text": "Meet me at the docs. " * 8,
                                               "icon": "CHAR_LESTER"})
            check(code == 202, "sms accepted")
            check((await ev.expect("sms_shown")) is not None, "sms_shown event")
            check(http("POST", "/sms", {"from": "x"})[0] == 400, "sms without text -> 400")
            check(http("POST", "/nope", {})[0] == 404, "unknown endpoint -> 404")

            print("tts call, answered, plays to completion")
            code, body = http("POST", "/call", {"from": "Lester", "text": "Hey. It's Lester. I have a job for you."})
            check(code == 202 and body.get("id"), "call accepted")
            cid = body["id"]
            check((await ev.expect("call_ringing"))["id"] == cid, "call_ringing")
            check(http("GET", "/status")[1]["call_state"] == "ringing", "status shows ringing")
            print("  busy")
            code, busy = http("POST", "/call", {"from": "Other", "text": "hi"})
            m = await ev.expect("call_rejected")
            check(m is not None and m.get("reason") == "busy", "second call rejected as busy")
            key("y")
            check((await ev.expect("call_answered")) is not None, "call_answered")
            check((await ev.expect("call_active")) is not None, "call_active (TTS rendered)")
            m = await ev.expect("call_ended", timeout=20)
            check(m is not None and m.get("reason") == "completed", f"call ended completed: {m}")

            print("declined")
            code, body = http("POST", "/call", {"from": "Martin", "text": "You there?"})
            await ev.expect("call_ringing")
            key("n")
            check((await ev.expect("call_declined")) is not None, "call_declined")
            m = await ev.expect("call_ended")
            check(m and m["reason"] == "declined", "ended: declined")

            print("missed")
            http("POST", "/call", {"from": "Martin", "text": "Hello?", "ring_timeout": 2})
            check((await ev.expect("call_missed", timeout=6)) is not None, "call_missed after timeout")

            print("audio_base64 clip + remote hangup")
            wav = base64.b64encode(make_wav(5.0)).decode()
            code, body = http("POST", "/call", {"from": "Trevor", "audio_base64": wav, "text": "Clip call."})
            check(code == 202, "clip call accepted")
            await ev.expect("call_ringing")
            key("y")
            check((await ev.expect("call_active")) is not None, "clip call active")
            await asyncio.sleep(1)
            check(http("POST", "/hangup", {"id": body["id"]})[0] == 202, "hangup accepted")
            m = await ev.expect("call_ended")
            check(m and m["reason"] == "remote_hung_up", f"ended: remote_hung_up ({m})")

            print("bad clip")
            http("POST", "/call", {"from": "X", "audio_base64": base64.b64encode(b"not audio" * 10).decode()})
            await ev.expect("call_ringing")
            key("y")
            m = await ev.expect("call_ended")
            check(m and m["reason"] == "media_failed", "undecodable clip -> media_failed")

            print("streaming call over WS with mic")
            check(http("POST", "/call", {"from": "AI", "stream": True})[0] == 400, "stream over HTTP rejected")
            await ws.send(json.dumps({"type": "call", "from": "AI", "stream": True, "sample_rate": 24000,
                                      "mic": True, "req_id": 7}))
            ack = await ev.expect("ack")
            check(ack and ack["ok"] and ack.get("req_id") == 7, "WS call ack")
            await ev.expect("call_ringing")
            key("y")
            m = await ev.expect("call_active")
            check(m and m.get("sample_rate") == 24000 and m.get("mic") is True, f"stream active {m}")
            frame = b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 440 * i / 24000))) for i in range(480))
            for _ in range(100):  # 2 s of 20 ms frames
                await ws.send(frame)
                await asyncio.sleep(0.02)
            await ws.send(json.dumps({"type": "subtitle", "text": "Streaming subtitle.", "duration_ms": 1500}))
            ack = await ev.expect("ack")
            check(ack and ack["ok"], "subtitle accepted during stream")
            print(f"  mic bytes received: {ev.binary_bytes}")
            check(ev.binary_bytes > 0, "received microphone audio (needs a capture device)")
            await ws.send(json.dumps({"type": "hangup"}))
            m = await ev.expect("call_ended")
            check(m and m["reason"] == "remote_hung_up", "stream call hung up via WS")

            print("owner disconnect ends call")
            async with websockets.connect(f"{WS}/?token={TOKEN}") as ws2:
                await ws2.recv()
                await ws2.send(json.dumps({"type": "call", "from": "AI", "stream": True}))
                await ev.expect("call_ringing")
                key("y")
                await ev.expect("call_active")
            m = await ev.expect("call_ended")
            check(m and m["reason"] == "remote_hung_up", "owner socket closed -> call ended")

            print("client killed without closing (crash regression)")
            killer = subprocess.Popen([sys.executable, "-c", f"""
import asyncio, websockets
async def m():
    async with websockets.connect('{WS}/?token={TOKEN}') as ws:
        await ws.recv(); print('connected', flush=True); await asyncio.sleep(3600)
asyncio.run(m())
"""], stdout=subprocess.PIPE, text=True)
            killer.stdout.readline()
            killer.kill()
            killer.wait()
            await asyncio.sleep(0.3)
            for i in range(3):
                check(http("POST", "/sms", {"from": "x", "text": f"after kill {i}"})[0] == 202, f"sms {i} after dead client")
                check((await ev.expect("sms_shown")) is not None, f"sms_shown {i} after dead client")
            check(proc.poll() is None, "plugin still running after a client died abruptly")

            pump.cancel()
    finally:
        key("q")
        proc.wait(5)

    print(f"\n{len(failures)} failure(s)")
    for f in failures:
        print("  -", f)
    sys.exit(1 if failures else 0)


asyncio.run(main())
