"""PhoneLink example client.

    pip install websockets

    python examples/phonelink_client.py sms  "Lester" "Meet me at the docks."
    python examples/phonelink_client.py call "Lester" "Hey, it's Lester. I have a job for you."
    python examples/phonelink_client.py play "Franklin" path/to/voice.wav   # pre-recorded / TTS file
    python examples/phonelink_client.py stream "AI" path/to/voice.wav       # live stream + record mic

Set PHONELINK_TOKEN if you configured a Token in PhoneLink.ini.
"""
import asyncio
import base64
import json
import os
import sys
import urllib.request
import wave

HTTP = os.environ.get("PHONELINK_HTTP", "http://127.0.0.1:8765")
WS = os.environ.get("PHONELINK_WS", "ws://127.0.0.1:8766")
TOKEN = os.environ.get("PHONELINK_TOKEN", "")


def post(path, body):
    req = urllib.request.Request(HTTP + path, data=json.dumps(body).encode(), method="POST")
    req.add_header("Content-Type", "application/json")
    if TOKEN:
        req.add_header("Authorization", f"Bearer {TOKEN}")
    with urllib.request.urlopen(req) as r:
        return json.loads(r.read())


async def stream_call(sender, wav_path):
    """Place a call, stream a 16-bit PCM WAV as if it were live, save the player's mic to mic.wav."""
    import websockets

    with wave.open(wav_path, "rb") as w:
        assert w.getsampwidth() == 2, "WAV must be 16-bit PCM"
        rate, channels, pcm = w.getframerate(), w.getnchannels(), w.readframes(w.getnframes())

    mic = bytearray()
    async with websockets.connect(f"{WS}/?token={TOKEN}", max_size=None) as ws:
        await ws.send(json.dumps({"type": "call", "from": sender, "stream": True,
                                  "sample_rate": rate, "channels": channels, "mic": True}))
        call_id = None
        async for msg in ws:
            if isinstance(msg, bytes):
                mic.extend(msg)
                continue
            ev = json.loads(msg)
            print(ev)
            if ev.get("event") == "ack":
                call_id = ev.get("id")
            elif ev.get("event") == "call_active" and ev.get("id") == call_id:
                break
            elif ev.get("event") == "call_ended" and ev.get("id") == call_id:
                return

        # Send 20 ms frames in real time (a real voice agent would send as audio is generated).
        frame = rate // 50 * channels * 2
        for i in range(0, len(pcm), frame):
            await ws.send(pcm[i:i + frame])
            await asyncio.sleep(0.02)
            while True:  # collect any mic audio / events that arrived meanwhile
                try:
                    msg = await asyncio.wait_for(ws.recv(), 0.001)
                except asyncio.TimeoutError:
                    break
                if isinstance(msg, bytes):
                    mic.extend(msg)
                elif json.loads(msg).get("event") == "call_ended":
                    print("player hung up")
                    break
        await asyncio.sleep(0.5)
        await ws.send(json.dumps({"type": "hangup", "id": call_id}))

    with wave.open("mic.wav", "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(bytes(mic))
    print(f"saved {len(mic) / 32000:.1f}s of player microphone audio to mic.wav")


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)
    cmd, sender, arg = sys.argv[1:4]
    if cmd == "sms":
        print(post("/sms", {"from": sender, "text": arg, "icon": "CHAR_LESTER"}))
    elif cmd == "call":
        print(post("/call", {"from": sender, "text": arg, "icon": "CHAR_LESTER"}))
    elif cmd == "play":
        with open(arg, "rb") as f:
            audio = base64.b64encode(f.read()).decode()
        print(post("/call", {"from": sender, "audio_base64": audio}))
    elif cmd == "stream":
        asyncio.run(stream_call(sender, arg))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
