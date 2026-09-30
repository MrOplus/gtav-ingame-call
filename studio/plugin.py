"""Client for the PhoneLink plugin running inside GTA V (HTTP + WebSocket on localhost)."""
from __future__ import annotations

import asyncio
import json
import logging
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Callable

import websockets

log = logging.getLogger("plugin")


class GameUnavailable(RuntimeError):
    pass


@dataclass
class PluginSettings:
    host: str = "127.0.0.1"
    http_port: int = 8765
    ws_port: int = 8766
    token: str = ""

    @property
    def http(self) -> str:
        return f"http://{self.host}:{self.http_port}"

    @property
    def ws(self) -> str:
        return f"ws://{self.host}:{self.ws_port}/" + (f"?token={self.token}" if self.token else "")


class Plugin:
    """Tracks game status, relays plugin events, and sends requests."""

    def __init__(self, settings: PluginSettings):
        self.settings = settings
        self.status: dict = {"connected": False}
        self.listeners: list[Callable[[dict], None]] = []
        self.on_connected: list[Callable[[], None]] = []
        self._ws = None  # persistent control connection (events, contacts)

    def on_event(self, fn: Callable[[dict], None]) -> None:
        self.listeners.append(fn)

    def _emit(self, ev: dict) -> None:
        for fn in list(self.listeners):
            fn(ev)

    @property
    def in_game(self) -> bool:
        return bool(self.status.get("connected") and self.status.get("in_game"))

    @property
    def idle(self) -> bool:
        return self.in_game and self.status.get("call_state", "idle") == "idle"

    async def listen_forever(self) -> None:
        while True:
            try:
                async with websockets.connect(self.settings.ws, open_timeout=3, ping_interval=10) as ws:
                    self._ws = ws
                    async for msg in ws:
                        if isinstance(msg, bytes):
                            continue
                        ev = json.loads(msg)
                        name = ev.get("event")
                        if name in ("hello", "status"):
                            was = self.status.get("connected")
                            self.status = {"connected": True, **{k: ev.get(k) for k in ("in_game", "call_state", "version")}}
                            if not was:
                                self._emit({"event": "game_connected", "version": ev.get("version")})
                                for fn in list(self.on_connected):
                                    fn()
                        elif name != "ack":
                            self._emit(ev)
                            if name.startswith("call_"):
                                await ws.send(json.dumps({"type": "status"}))
            except (OSError, websockets.WebSocketException, asyncio.TimeoutError):
                pass
            self._ws = None
            if self.status.get("connected"):
                self._emit({"event": "game_disconnected"})
            self.status = {"connected": False}
            await asyncio.sleep(3)

    async def send(self, msg: dict) -> bool:
        """Send a command over the persistent control connection (e.g. the contacts list)."""
        ws = self._ws
        if ws is None:
            return False
        try:
            await ws.send(json.dumps(msg))
            return True
        except websockets.WebSocketException:
            return False

    async def poll_status(self) -> None:
        while True:
            await asyncio.sleep(5)
            if self.status.get("connected"):
                try:
                    s = await self.request("GET", "/status")
                    self.status = {"connected": True, **{k: s.get(k) for k in ("in_game", "call_state", "version")}}
                except GameUnavailable:
                    pass

    async def request(self, method: str, path: str, body: dict | None = None) -> dict:
        def do():
            req = urllib.request.Request(self.settings.http + path, method=method,
                                         data=json.dumps(body).encode() if body is not None else None)
            req.add_header("Content-Type", "application/json")
            if self.settings.token:
                req.add_header("Authorization", f"Bearer {self.settings.token}")
            try:
                with urllib.request.urlopen(req, timeout=5) as r:
                    return json.loads(r.read() or b"{}")
            except urllib.error.HTTPError as e:
                detail = json.loads(e.read() or b"{}").get("error", e.reason)
                raise RuntimeError(f"game refused the request: {detail}") from None
            except OSError:
                raise GameUnavailable("game not reachable - is GTA V running with PhoneLink installed?") from None

        return await asyncio.to_thread(do)

    async def sms(self, sender: str, text: str, icon: str, subject: str = "") -> dict:
        return await self.request("POST", "/sms", {"from": sender, "text": text, "icon": icon, "subject": subject})

    async def test_call(self, sender: str, text: str) -> dict:
        return await self.request("POST", "/call", {"from": sender, "text": text})

    async def hangup(self) -> dict:
        return await self.request("POST", "/hangup", {})

    async def open_call(self, sender: str, icon: str, sample_rate: int, mic_rate: int,
                        outgoing: bool = False) -> "PluginCall":
        call = PluginCall(self.settings.ws)
        await call.open(sender, icon, sample_rate, mic_rate, outgoing)
        return call


class PluginCall:
    """One live streamed call: audio in both directions over its own WebSocket."""

    def __init__(self, url: str):
        self.url = url
        self.ws = None
        self.id: str | None = None
        self.events: asyncio.Queue = asyncio.Queue()
        self.on_mic: Callable[[bytes], None] | None = None
        self._reader: asyncio.Task | None = None
        self.opened_at = time.monotonic()

    async def open(self, sender: str, icon: str, sample_rate: int, mic_rate: int, outgoing: bool = False) -> None:
        try:
            self.ws = await websockets.connect(self.url, max_size=None, open_timeout=3, ping_interval=10)
        except (OSError, websockets.WebSocketException, asyncio.TimeoutError):
            raise GameUnavailable("game not reachable") from None
        await self.ws.recv()  # hello
        await self.ws.send(json.dumps({"type": "call", "from": sender, "icon": icon, "stream": True,
                                       "sample_rate": sample_rate, "channels": 1, "mic": True,
                                       "mic_sample_rate": mic_rate, "outgoing": outgoing}))
        while self.id is None:
            ev = json.loads(await self.ws.recv())
            if ev.get("event") == "ack":
                if not ev.get("ok"):
                    raise RuntimeError(ev.get("error", "call refused"))
                self.id = ev["id"]
        self._reader = asyncio.create_task(self._read())

    async def _read(self) -> None:
        try:
            async for msg in self.ws:
                if isinstance(msg, bytes):
                    if self.on_mic:
                        self.on_mic(msg)
                    continue
                ev = json.loads(msg)
                if ev.get("id") == self.id and ev.get("event", "").startswith("call_"):
                    await self.events.put(ev)
        except websockets.WebSocketException:
            pass
        await self.events.put({"event": "call_ended", "reason": "game_disconnected"})

    async def send_audio(self, pcm: bytes) -> None:
        try:
            await self.ws.send(pcm)
        except websockets.WebSocketException:
            pass

    async def hangup(self, reason: str = "") -> None:
        try:
            await self.ws.send(json.dumps({"type": "hangup", "id": self.id, "reason": reason}))
        except websockets.WebSocketException:
            pass

    async def connect(self) -> None:
        """Outgoing call: the friend picked up; switch the in-game phone to the active call."""
        try:
            await self.ws.send(json.dumps({"type": "connect", "id": self.id}))
        except websockets.WebSocketException:
            pass

    async def close(self) -> None:
        if self._reader:
            self._reader.cancel()
        if self.ws:
            await self.ws.close()
