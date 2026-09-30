"""Game-exit regression test: the plugin must not crash or hang when the process exits
while its threads (servers, watchdog, sender, audio, a live call) are running.

    python tests/harness/exit_test.py build/harness
"""
import asyncio
import json
import os
import subprocess
import sys
import time
import urllib.request

hd = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/harness")
failures = 0
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
open(os.path.join(hd, "PhoneLink.ini"), "w").write(HARNESS_INI)


def post(path, body):
    req = urllib.request.Request("http://127.0.0.1:18765" + path, data=json.dumps(body).encode(), method="POST",
                                 headers={"Content-Type": "application/json", "Authorization": "Bearer s3cret"})
    return urllib.request.urlopen(req, timeout=3).status


async def open_stream_call():
    import websockets
    ws = await websockets.connect("ws://127.0.0.1:18766/?token=s3cret")
    await ws.recv()
    await ws.send(json.dumps({"type": "call", "from": "Exit test", "stream": True, "sample_rate": 48000,
                              "mic": True, "mic_sample_rate": 48000}))
    return ws


loop = asyncio.new_event_loop()
for scenario in ("idle", "during TTS call", "during live stream call with mic"):
    p = subprocess.Popen([os.path.join(hd, "harness.exe")], cwd=hd, stdin=subprocess.PIPE,
                         stdout=subprocess.DEVNULL, text=True)
    time.sleep(1.5)
    if scenario.startswith("during live"):
        ws = loop.run_until_complete(open_stream_call())
        time.sleep(0.5)
        p.stdin.write("y\n")
        p.stdin.flush()
        for _ in range(50):  # 1 s of audio into the game while the mic streams back
            loop.run_until_complete(ws.send(bytes(1920)))
            time.sleep(0.02)
    elif scenario != "idle":
        post("/call", {"from": "Exit test", "text": "This call is still going when the game quits. " * 3})
        time.sleep(0.5)
        p.stdin.write("y\n")
        p.stdin.flush()
        time.sleep(1.0)
    p.stdin.write("exit\n")
    p.stdin.flush()
    try:
        code = p.wait(10)
        ok = code == 0
        print(("PASS" if ok else "FAIL") + f" {scenario}: exit code {code} (0x{code & 0xFFFFFFFF:08X})")
    except subprocess.TimeoutExpired:
        ok = False
        print(f"FAIL {scenario}: process hung on exit")
        p.kill()
    failures += not ok
    time.sleep(0.5)

sys.exit(1 if failures else 0)
