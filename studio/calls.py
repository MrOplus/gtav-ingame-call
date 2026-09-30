"""Incoming WebRTC calls from friends' browsers, bridged into the game through the plugin.

Caller browser  <-- WebRTC (Opus) -->  Studio (aiortc)  <-- PCM over WS -->  PhoneLink.asi
"""
from __future__ import annotations

import asyncio
import fractions
import json
import logging
import secrets
import time
from dataclasses import dataclass, field
from typing import Callable

import av
from aiortc import MediaStreamTrack, RTCConfiguration, RTCIceServer, RTCPeerConnection, RTCSessionDescription
from fastapi import WebSocket, WebSocketDisconnect

from plugin import GameUnavailable, Plugin, PluginCall

log = logging.getLogger("calls")

RATE = 48000
FRAME = 960  # 20 ms


def _limit_ice_to_primary_interface() -> None:
    """aioice gathers on every adapter (VPN, Hyper-V, WSL, IPv6...) and waits ~5 s for STUN
    timeouts on those without internet. Only the adapter that routes to the internet matters."""
    import socket

    import aioice.ice

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("1.1.1.1", 53))  # no packets are sent; this just picks the outbound route
        primary = s.getsockname()[0]
    except OSError:
        return
    finally:
        s.close()
    aioice.ice.get_host_addresses = lambda use_ipv4, use_ipv6: [primary]
    log.info("WebRTC uses network address %s", primary)


_limit_ice_to_primary_interface()


@dataclass
class CallSettings:
    display_name: str = "Me"
    invite_code: str = ""
    public_url: str = ""
    caller_icon: str = "CHAR_DEFAULT"
    stun_urls: list[str] = field(default_factory=lambda: ["stun:stun.cloudflare.com:3478", "stun:stun.l.google.com:19302"])
    turn_urls: str = ""  # comma-separated, e.g. "turn:turn.example.com:3478?transport=udp"
    turn_username: str = ""
    turn_credential: str = ""

    def ice_servers(self) -> list[dict]:
        servers = [{"urls": u} for u in self.stun_urls if u]
        turn = [u.strip() for u in self.turn_urls.split(",") if u.strip()]
        if turn:
            servers.append({"urls": turn, "username": self.turn_username, "credential": self.turn_credential})
        return servers

    def invite_link(self) -> str:
        base = self.public_url.rstrip("/") if self.public_url else "https://<your-tunnel-hostname>"
        return f"{base}/?code={self.invite_code}"


def clean_name(name) -> str:
    return "".join(ch for ch in str(name or "").strip() if ch.isprintable())[:24]


class PlayerMicTrack(MediaStreamTrack):
    """Sends the player's microphone (PCM pushed from the plugin) to the caller, paced at 20 ms."""

    kind = "audio"

    def __init__(self):
        super().__init__()
        self.buf = bytearray()
        self._start: float | None = None
        self._pts = 0

    def push(self, pcm: bytes) -> None:
        self.buf.extend(pcm)
        max_bytes = RATE * 2 // 5  # 200 ms: beyond that we are lagging; drop the oldest audio
        if len(self.buf) > max_bytes:
            del self.buf[: len(self.buf) - RATE * 2 // 16]

    async def recv(self) -> av.AudioFrame:
        if self._start is None:
            self._start = time.monotonic()
        else:
            wait = self._start + self._pts / RATE - time.monotonic()
            if wait > 0:
                await asyncio.sleep(wait)
        need = FRAME * 2
        chunk = bytes(self.buf[:need])
        del self.buf[:need]
        if len(chunk) < need:
            chunk += b"\x00" * (need - len(chunk))
        frame = av.AudioFrame(format="s16", layout="mono", samples=FRAME)
        frame.planes[0].update(chunk)
        frame.sample_rate = RATE
        frame.pts = self._pts
        frame.time_base = fractions.Fraction(1, RATE)
        self._pts += FRAME
        return frame


RING_TIMEOUT_SEC = 30  # outgoing: how long a friend's page rings before "no answer"
SMS_MAX_CHARS = 300
SMS_MIN_INTERVAL_SEC = 1.5


class Friend:
    """One open call page (a friend who can call you, text you, and be called)."""

    def __init__(self, ws: WebSocket, name: str, ip: str):
        self.id = secrets.token_hex(4)
        self.ws = ws
        self.name = name
        self.ip = ip
        self.inbox: asyncio.Queue = asyncio.Queue()  # outgoing calls handed over by the manager
        self.last_sms = 0.0

    async def send(self, msg: dict) -> None:
        try:
            await self.ws.send_text(json.dumps(msg))
        except Exception:
            pass


class CallSession:
    """One call between the player and a friend, in either direction."""

    def __init__(self, manager: "CallManager", friend: Friend, outgoing: bool, plugin_call: PluginCall | None = None):
        self.manager = manager
        self.friend = friend
        self.caller = friend.name
        self.outgoing = outgoing
        self.plugin_call = plugin_call
        self.pc: RTCPeerConnection | None = None
        self.mic = PlayerMicTrack()
        self.connected_at: float | None = None
        self.answered = asyncio.Event()
        self.accept: asyncio.Future = asyncio.get_running_loop().create_future()  # outgoing: offer SDP or None
        self.game_reason = ""  # reason shown in-game when Studio ends the call
        self.tasks: list[asyncio.Task] = []

    async def send(self, msg: dict) -> None:
        await self.friend.send(msg)

    async def run_incoming(self, offer_sdp: str) -> str:
        """Friend calls the player."""
        s = self.manager.settings
        try:
            self.plugin_call = await self.manager.plugin.open_call(self.caller, s.caller_icon, RATE, RATE)
        except GameUnavailable:
            return "offline"
        except RuntimeError as e:
            return "busy" if "busy" in str(e) else "failed"
        return await self._negotiate(offer_sdp)

    async def run_outgoing(self) -> str:
        """Player calls the friend: the game already shows 'Calling...'; ring the friend's page."""
        await self.send({"type": "incoming", "from": self.manager.settings.display_name})
        game_ended = asyncio.create_task(self._wait_game_end())
        done, _ = await asyncio.wait({self.accept, game_ended}, timeout=RING_TIMEOUT_SEC,
                                     return_when=asyncio.FIRST_COMPLETED)
        if game_ended in done:
            return "cancelled"  # the player hung up while it was ringing
        game_ended.cancel()
        if not done:
            self.game_reason = "no_answer"
            return "no_answer"
        offer = self.accept.result()
        if offer is None:
            self.game_reason = "declined"
            return "declined"
        return await self._negotiate(offer)

    async def _wait_game_end(self) -> None:
        while True:
            ev = await self.plugin_call.events.get()
            if ev.get("event") == "call_ended":
                return

    async def _negotiate(self, offer_sdp: str) -> str:
        s = self.manager.settings
        self.plugin_call.on_mic = self.mic.push
        ice = [RTCIceServer(urls=i["urls"], username=i.get("username"), credential=i.get("credential"))
               for i in s.ice_servers()]
        self.pc = RTCPeerConnection(RTCConfiguration(iceServers=ice))

        @self.pc.on("track")
        def on_track(track):
            if track.kind == "audio":
                self.tasks.append(asyncio.create_task(self._forward_caller_audio(track)))

        @self.pc.on("connectionstatechange")
        async def on_state():
            log.info("call with %s: webrtc %s", self.caller, self.pc.connectionState)
            if self.pc.connectionState == "connected" and self.outgoing:
                await self.plugin_call.connect()  # friend picked up and media flows: go live in-game
            elif self.pc.connectionState == "failed":
                self.game_reason = "unavailable" if self.outgoing and not self.answered.is_set() else ""
                await self.plugin_call.hangup(self.game_reason)

        await self.pc.setRemoteDescription(RTCSessionDescription(sdp=offer_sdp, type="offer"))
        self.pc.addTrack(self.mic)
        await self.pc.setLocalDescription(await self.pc.createAnswer())  # gathers ICE candidates
        await self.send({"type": "answer", "sdp": self.pc.localDescription.sdp})

        # Follow the in-game call until it ends.
        while True:
            ev = await self.plugin_call.events.get()
            name = ev.get("event")
            if name == "call_ringing":
                await self.send({"type": "ringing"})
            elif name == "call_active":
                self.connected_at = time.monotonic()
                self.answered.set()
                await self.send({"type": "connected"})
            elif name == "call_rejected":
                return "busy"
            elif name == "call_declined":
                return "declined"
            elif name == "call_missed":
                return "no_answer"
            elif name == "call_ended":
                if self.pc and self.pc.connectionState == "failed":
                    return "connection_failed"
                return {"player_hung_up": "host_hung_up", "remote_hung_up": "hung_up", "declined": "declined",
                        "missed": "no_answer", "no_answer": "no_answer", "unavailable": "connection_failed",
                        "player_unavailable": "host_unavailable",
                        "game_disconnected": "host_unavailable"}.get(ev.get("reason", ""), "ended")

    async def _forward_caller_audio(self, track) -> None:
        resampler = av.AudioResampler(format="s16", layout="mono", rate=RATE)
        while True:
            try:
                frame = await track.recv()
            except Exception:
                return
            if not self.answered.is_set():
                continue  # not live in-game yet
            pcm = b"".join(bytes(f.planes[0])[: f.samples * 2] for f in resampler.resample(frame))
            await self.plugin_call.send_audio(pcm)

    async def close(self, hangup_game: bool) -> None:
        for t in self.tasks:
            t.cancel()
        if self.plugin_call:
            if hangup_game:
                await self.plugin_call.hangup(self.game_reason)
            await self.plugin_call.close()
        if self.pc:
            await self.pc.close()


class CallManager:
    def __init__(self, plugin: Plugin, settings: CallSettings, publish: Callable[[dict], None]):
        self.plugin = plugin
        self.settings = settings
        self.publish = publish
        self.active: CallSession | None = None
        self.friends: dict[str, Friend] = {}
        self.history: list[dict] = []
        self.failures: dict[str, list[float]] = {}
        plugin.on_event(self._on_plugin_event)
        plugin.on_connected.append(lambda: asyncio.ensure_future(self.push_contacts()))

    # ---- presence ------------------------------------------------------------------
    def availability(self) -> str:
        if self.active is not None or (self.plugin.in_game and not self.plugin.idle):
            return "busy"
        return "available" if self.plugin.in_game else "offline"

    def contacts(self) -> list[dict]:
        return [{"id": f.id, "name": f.name} for f in self.friends.values() if f.name]

    async def push_contacts(self) -> None:
        await self.plugin.send({"type": "contacts", "contacts": self.contacts()})

    def _rate_limited(self, ip: str) -> bool:
        now = time.monotonic()
        recent = [t for t in self.failures.get(ip, []) if now - t < 600]
        self.failures[ip] = recent
        return len(recent) >= 5

    async def hangup_active(self) -> bool:
        if self.active and self.active.plugin_call:
            await self.active.plugin_call.hangup()
            return True
        return False

    # ---- player calls a friend (in-game contacts menu) ---------------------------------
    def _on_plugin_event(self, ev: dict) -> None:
        if ev.get("event") == "dial":
            asyncio.ensure_future(self.dial(ev.get("contact", "")))

    async def dial(self, contact_id: str) -> None:
        friend = self.friends.get(contact_id)
        if friend is None or self.active is not None:
            who = friend.name if friend else "That contact"
            log.info("dial %s: %s", contact_id, "busy" if friend else "not online")
            try:
                await self.plugin.sms(who, who + " is not available right now.", self.settings.caller_icon, "Call failed")
            except Exception:
                pass
            return
        try:
            plugin_call = await self.plugin.open_call(friend.name, self.settings.caller_icon, RATE, RATE, outgoing=True)
        except Exception as e:
            log.warning("dial %s failed: %s", friend.name, e)
            return
        session = CallSession(self, friend, outgoing=True, plugin_call=plugin_call)
        self.active = session
        log.info("player is calling %s (%s)", friend.name, friend.ip)
        self.publish({"event": "calling_friend", "to": friend.name})
        await friend.inbox.put(session)

    # ---- one friend's call page ----------------------------------------------------------
    async def handle(self, ws: WebSocket) -> None:
        """Signaling for one call page (public, reached through the Cloudflare tunnel)."""
        await ws.accept()
        ip = ws.headers.get("cf-connecting-ip") or (ws.client.host if ws.client else "?")
        friend: Friend | None = None
        session: CallSession | None = None
        call_task: asyncio.Task | None = None
        receive: asyncio.Task | None = None
        inbox: asyncio.Task | None = None
        try:
            hello = json.loads(await asyncio.wait_for(ws.receive_text(), 15))
            if self._rate_limited(ip):
                await ws.send_text(json.dumps({"type": "denied", "reason": "rate_limited"}))
                return
            if hello.get("type") != "hello" or not self.settings.invite_code or hello.get("code") != self.settings.invite_code:
                self.failures.setdefault(ip, []).append(time.monotonic())
                log.warning("rejected caller from %s: bad invite code", ip)
                await ws.send_text(json.dumps({"type": "denied", "reason": "bad_code"}))
                return
            friend = Friend(ws, clean_name(hello.get("name")), ip)
            self.friends[friend.id] = friend
            await self.push_contacts()
            await friend.send({"type": "welcome", "host": self.settings.display_name,
                               "status": self.availability(), "iceServers": self.settings.ice_servers()})

            while True:
                receive = receive or asyncio.create_task(ws.receive_text())
                inbox = inbox or asyncio.create_task(friend.inbox.get())
                waits = {receive, inbox} | ({call_task} if call_task else set())
                done, _ = await asyncio.wait(waits, return_when=asyncio.FIRST_COMPLETED)

                if call_task in done:
                    try:
                        result = call_task.result()
                    except Exception:
                        log.exception("call failed")
                        result = "failed"
                    await self._finish(session, result, hangup_game=result in ("failed", "no_answer", "declined"))
                    session, call_task = None, None

                if inbox in done:
                    new_session: CallSession = inbox.result()
                    inbox = None
                    if call_task is None:
                        session = new_session
                        call_task = asyncio.create_task(session.run_outgoing())
                    else:  # already on a call with this page
                        new_session.game_reason = "unavailable"
                        await new_session.close(hangup_game=True)
                        if self.active is new_session:
                            self.active = None

                if receive not in done:
                    continue
                msg = json.loads(receive.result())
                receive = None
                await self._on_message(friend, msg, session)
                kind = msg.get("type")
                if kind == "call" and call_task is None:
                    friend.name = clean_name(msg.get("name")) or friend.name or "Friend"
                    status = self.availability()
                    if status != "available":
                        await friend.send({"type": "ended", "reason": "busy" if status == "busy" else "offline"})
                        self._record(friend.name, "busy" if status == "busy" else "offline", 0, "in")
                        continue
                    session = CallSession(self, friend, outgoing=False)
                    self.active = session
                    log.info("incoming call from %s (%s)", friend.name, ip)
                    self.publish({"event": "friend_calling", "from": friend.name})
                    call_task = asyncio.create_task(session.run_incoming(msg["sdp"]))
                elif kind == "hangup" and call_task:
                    call_task.cancel()
                    if session.outgoing and not session.answered.is_set():
                        session.game_reason = "declined"
                    await self._finish(session, "hung_up", hangup_game=True)
                    session, call_task = None, None
        except (WebSocketDisconnect, asyncio.TimeoutError, json.JSONDecodeError, KeyError):
            pass
        except Exception:
            log.exception("signaling error")
        finally:
            for t in (receive, inbox):
                if t:
                    t.cancel()
            if call_task:
                call_task.cancel()
                if session and session.outgoing and not session.answered.is_set():
                    session.game_reason = "unavailable"
                await self._finish(session, "caller_left", hangup_game=True)
            if friend:
                self.friends.pop(friend.id, None)
                await self.push_contacts()

    async def _on_message(self, friend: Friend, msg: dict, session: CallSession | None) -> None:
        kind = msg.get("type")
        if kind == "status":
            await friend.send({"type": "status", "status": self.availability()})
        elif kind == "profile":
            name = clean_name(msg.get("name"))
            if name and name != friend.name:
                friend.name = name
                await self.push_contacts()
        elif kind in ("accept", "decline") and session and session.outgoing and not session.accept.done():
            session.accept.set_result(msg.get("sdp") if kind == "accept" else None)
        elif kind == "sms":
            await self._sms(friend, msg)

    async def _sms(self, friend: Friend, msg: dict) -> None:
        text = str(msg.get("text", "")).strip()[:SMS_MAX_CHARS]
        now = time.monotonic()
        if not text:
            return
        if now - friend.last_sms < SMS_MIN_INTERVAL_SEC:
            await friend.send({"type": "sms_result", "ok": False, "reason": "slow_down"})
            return
        if not self.plugin.in_game:
            await friend.send({"type": "sms_result", "ok": False, "reason": "offline"})
            return
        friend.last_sms = now
        name = clean_name(msg.get("name")) or friend.name or "Friend"
        try:
            await self.plugin.sms(name, text, self.settings.caller_icon, "Message")
        except Exception:
            await friend.send({"type": "sms_result", "ok": False, "reason": "offline"})
            return
        log.info("text from %s (%d chars)", name, len(text))
        self.publish({"event": "friend_sms", "from": name, "text": text})
        await friend.send({"type": "sms_result", "ok": True})

    async def _finish(self, session: CallSession | None, result: str, hangup_game: bool) -> None:
        if session is None:
            return
        duration = time.monotonic() - session.connected_at if session.connected_at else 0
        await session.send({"type": "ended", "reason": result})
        await session.close(hangup_game)
        if self.active is session:
            self.active = None
        log.info("call %s %s finished: %s (%.0fs)", "to" if session.outgoing else "from", session.caller, result, duration)
        self._record(session.caller, result, duration, "out" if session.outgoing else "in")

    def _record(self, name: str, result: str, duration: float, direction: str) -> None:
        entry = {"time": time.strftime("%H:%M:%S"), "from": name, "result": result, "duration": round(duration),
                 "direction": direction}
        self.history = ([entry] + self.history)[:50]
        self.publish({"event": "friend_call_ended", **entry})
