"""PhoneLink Studio: control panel + friend-calling server for the PhoneLink GTA V plugin.

    python app.py [--no-browser]

Two local servers:
  http://127.0.0.1:8770  control panel (private - never expose this)
  http://127.0.0.1:8771  friends' call page + signaling (expose this one through Cloudflare Tunnel)
"""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import logging
import os
import secrets
import shutil
import subprocess
import sys
import threading
import time
import webbrowser
from dataclasses import asdict
from pathlib import Path

from fastapi import FastAPI, HTTPException, WebSocket
from fastapi.responses import FileResponse, StreamingResponse
from pydantic import BaseModel

ROOT = Path(__file__).resolve().parent
# PHONELINK_STUDIO_SETTINGS lets tests run a second instance with its own settings and log.
SETTINGS_FILE = Path(os.environ.get("PHONELINK_STUDIO_SETTINGS", ROOT / "settings.json"))
LOG_FILE = SETTINGS_FILE.parent / "studio.log"
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)-5s %(name)s: %(message)s",
                    handlers=[logging.StreamHandler(sys.stdout),
                              logging.FileHandler(LOG_FILE, encoding="utf-8")])
logging.getLogger("aioice").setLevel(logging.WARNING)
log = logging.getLogger("studio")

from calls import CallManager, CallSettings  # noqa: E402
from plugin import GameUnavailable, Plugin, PluginSettings  # noqa: E402

ICONS = ["CHAR_DEFAULT", "CHAR_LESTER", "CHAR_FRANKLIN", "CHAR_MICHAEL", "CHAR_TREVOR", "CHAR_LAMAR", "CHAR_AMANDA",
         "CHAR_JIMMY", "CHAR_TRACEY", "CHAR_DAVE", "CHAR_MARTIN", "CHAR_RON", "CHAR_WADE", "CHAR_SIMEON",
         "CHAR_DEVIN", "CHAR_STEVE", "CHAR_TANISHA", "CHAR_DENISE", "CHAR_CHOP", "CHAR_MULTIPLAYER"]


# ---- settings ------------------------------------------------------------------
def load_settings() -> tuple[PluginSettings, CallSettings, str]:
    data = json.loads(SETTINGS_FILE.read_text(encoding="utf-8")) if SETTINGS_FILE.exists() else {}
    ps = PluginSettings(**{k: v for k, v in data.get("plugin", {}).items() if k in PluginSettings.__dataclass_fields__})
    cs = CallSettings(**{k: v for k, v in data.get("calls", {}).items() if k in CallSettings.__dataclass_fields__})
    if not cs.invite_code:
        cs.invite_code = secrets.token_urlsafe(8)
    return ps, cs, data.get("gta_dir", "")


plugin_settings, call_settings, gta_dir_setting = load_settings()


def save_settings() -> None:
    SETTINGS_FILE.write_text(json.dumps({"plugin": asdict(plugin_settings), "calls": asdict(call_settings),
                                         "gta_dir": gta_dir_setting}, indent=2), encoding="utf-8")


save_settings()  # persist a generated invite code

# ---- events for the control panel -----------------------------------------------
subscribers: set[asyncio.Queue] = set()
history: list[dict] = []


def publish(ev: dict) -> None:
    global history
    ev.setdefault("time", time.strftime("%H:%M:%S"))
    history = (history + [ev])[-200:]
    for q in list(subscribers):
        q.put_nowait(ev)


plugin = Plugin(plugin_settings)
plugin.on_event(publish)
calls = CallManager(plugin, call_settings, publish)


# ---- GTA install detection / plugin install --------------------------------------
def plugin_source() -> Path | None:
    for p in (ROOT / "plugin" / "PhoneLink.asi", ROOT.parent / "build" / "PhoneLink.asi"):
        if p.exists():
            return p
    return None


def detect_gta_dirs() -> list[str]:
    found = []
    try:
        import winreg
        for sub, value in [(r"SOFTWARE\WOW6432Node\Rockstar Games\Grand Theft Auto V", "InstallFolder"),
                           (r"SOFTWARE\WOW6432Node\Rockstar Games\GTAV", "InstallFolderSteam"),
                           (r"SOFTWARE\WOW6432Node\Rockstar Games\GTAV", "InstallFolderEpic")]:
            try:
                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, sub) as k:
                    found.append(winreg.QueryValueEx(k, value)[0])
            except OSError:
                pass
    except ImportError:
        pass
    for drive in "CDEFGH":
        for rel in (r"GTAV", r"Program Files\Rockstar Games\Grand Theft Auto V",
                    r"Program Files (x86)\Steam\steamapps\common\Grand Theft Auto V",
                    r"Program Files\Epic Games\GTAV"):
            found.append(f"{drive}:\\{rel}")
    out = []
    for d in found:
        d = str(Path(d.rstrip("\\/")))
        if d not in out and (Path(d) / "GTA5.exe").exists():
            out.append(d)
    return out


def game_running() -> bool:
    try:
        r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq GTA5.exe", "/NH"], capture_output=True, text=True,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        return "GTA5.exe" in r.stdout
    except OSError:
        return False


def sha1(p: Path) -> str:
    return hashlib.sha1(p.read_bytes()).hexdigest()


def gta_info(path: str) -> dict:
    d = Path(path)
    src = plugin_source()
    asi = d / "PhoneLink.asi"
    return {
        "path": str(d),
        "valid": (d / "GTA5.exe").exists(),
        "scripthookv": (d / "ScriptHookV.dll").exists(),
        "asi_loader": (d / "dinput8.dll").exists(),
        "plugin_installed": asi.exists(),
        "plugin_up_to_date": asi.exists() and src is not None and sha1(asi) == sha1(src),
        "plugin_available": src is not None,
    }


def install_plugin(path: str) -> dict:
    d = Path(path)
    info = gta_info(path)
    if not info["valid"]:
        raise RuntimeError("GTA5.exe not found in that folder")
    if not info["scripthookv"] or not info["asi_loader"]:
        raise RuntimeError("install Script Hook V first (ScriptHookV.dll and dinput8.dll from dev-c.com)")
    src = plugin_source()
    if src is None:
        raise RuntimeError("PhoneLink.asi is missing from this package")
    try:
        shutil.copy2(src, d / "PhoneLink.asi")
    except PermissionError:
        raise RuntimeError("PhoneLink.asi is in use - close GTA V (check Task Manager for a stuck GTA5.exe)") from None
    for ini_src in (src.parent / "PhoneLink.ini", ROOT.parent / "PhoneLink.ini"):
        if ini_src.exists() and not (d / "PhoneLink.ini").exists():
            shutil.copy2(ini_src, d / "PhoneLink.ini")
            break
    log.info("installed plugin into %s", d)
    return gta_info(path)


# ---- request models (module level so FastAPI can resolve them) ---------------------
class SmsRequest(BaseModel):
    sender: str
    text: str
    icon: str = "CHAR_DEFAULT"
    subject: str = ""


class TestCallRequest(BaseModel):
    sender: str = "PhoneLink"
    text: str = "This is a test call from PhoneLink Studio."


class PluginSettingsRequest(BaseModel):
    host: str
    http_port: int
    ws_port: int
    token: str = ""


class CallSettingsRequest(BaseModel):
    display_name: str
    public_url: str = ""
    caller_icon: str = "CHAR_DEFAULT"
    turn_urls: str = ""
    turn_username: str = ""
    turn_credential: str = ""
    regenerate_code: bool = False


class InstallRequest(BaseModel):
    path: str


def fail(e: Exception, code: int = 400):
    raise HTTPException(code, str(e))


# ---- control panel (private) -------------------------------------------------------
def make_control_app() -> FastAPI:
    app = FastAPI(title="PhoneLink Studio")

    @app.get("/")
    async def index():
        return FileResponse(ROOT / "static" / "control.html")

    @app.get("/api/status")
    async def status():
        active = calls.active
        return {"game": plugin.status, "availability": calls.availability(),
                "active_call": {"from": active.caller, "connected": active.connected_at is not None} if active else None,
                "calls": {**asdict(call_settings), "invite_link": call_settings.invite_link()},
                "plugin": asdict(plugin_settings), "icons": ICONS, "history": calls.history,
                "friends": calls.contacts()}

    @app.get("/api/events")
    async def events():
        q: asyncio.Queue = asyncio.Queue()
        subscribers.add(q)

        async def gen():
            try:
                for ev in history[-30:]:
                    yield f"data: {json.dumps(ev)}\n\n"
                while True:
                    try:
                        yield f"data: {json.dumps(await asyncio.wait_for(q.get(), 15))}\n\n"
                    except asyncio.TimeoutError:
                        yield ": keep-alive\n\n"
            finally:
                subscribers.discard(q)

        return StreamingResponse(gen(), media_type="text/event-stream")

    @app.post("/api/sms")
    async def sms(req: SmsRequest):
        try:
            return await plugin.sms(req.sender, req.text, req.icon, req.subject)
        except Exception as e:
            fail(e)

    @app.post("/api/test-call")
    async def test_call(req: TestCallRequest):
        try:
            return await plugin.test_call(req.sender, req.text)
        except Exception as e:
            fail(e)

    @app.post("/api/hangup")
    async def hangup():
        if await calls.hangup_active():
            return {"ok": True}
        try:
            return await plugin.hangup()
        except GameUnavailable as e:
            fail(e)

    @app.post("/api/settings/plugin")
    async def set_plugin(req: PluginSettingsRequest):
        plugin_settings.host, plugin_settings.http_port = req.host, req.http_port
        plugin_settings.ws_port, plugin_settings.token = req.ws_port, req.token
        save_settings()
        return asdict(plugin_settings)

    @app.post("/api/settings/calls")
    async def set_calls(req: CallSettingsRequest):
        call_settings.display_name = req.display_name.strip()[:24] or "Me"
        call_settings.public_url = req.public_url.strip().rstrip("/")
        call_settings.caller_icon = req.caller_icon
        call_settings.turn_urls, call_settings.turn_username = req.turn_urls.strip(), req.turn_username.strip()
        call_settings.turn_credential = req.turn_credential
        if req.regenerate_code:
            call_settings.invite_code = secrets.token_urlsafe(8)
        save_settings()
        return {**asdict(call_settings), "invite_link": call_settings.invite_link()}

    @app.get("/api/gta")
    async def gta():
        dirs = detect_gta_dirs()
        if gta_dir_setting and gta_dir_setting not in dirs:
            dirs.insert(0, gta_dir_setting)
        return {"dirs": [gta_info(d) for d in dirs], "selected": gta_dir_setting or (dirs[0] if dirs else ""),
                "game_running": await asyncio.to_thread(game_running)}

    @app.post("/api/gta/install")
    async def gta_install(req: InstallRequest):
        global gta_dir_setting
        try:
            info = install_plugin(req.path)
            gta_dir_setting = req.path
            save_settings()
            return info
        except Exception as e:
            fail(e)

    @app.get("/api/log")
    async def get_log():
        p = LOG_FILE
        lines = p.read_text(encoding="utf-8", errors="replace").splitlines()[-200:] if p.exists() else []
        return {"lines": lines}

    return app


# ---- friends' call page (public, via tunnel) -------------------------------------------
def make_public_app() -> FastAPI:
    app = FastAPI(title="PhoneLink", docs_url=None, redoc_url=None, openapi_url=None)

    @app.get("/")
    async def call_page():
        return FileResponse(ROOT / "public" / "call.html", headers={"Cache-Control": "no-store"})

    @app.websocket("/signal")
    async def signal(ws: WebSocket):
        await calls.handle(ws)

    return app


async def serve(control_port: int, public_port: int) -> None:
    import uvicorn

    servers = [
        uvicorn.Server(uvicorn.Config(make_control_app(), host="127.0.0.1", port=control_port, log_level="warning")),
        uvicorn.Server(uvicorn.Config(make_public_app(), host="127.0.0.1", port=public_port, log_level="warning",
                                      proxy_headers=True)),
    ]
    tasks = [asyncio.create_task(plugin.listen_forever()), asyncio.create_task(plugin.poll_status())]
    try:
        await asyncio.gather(*(s.serve() for s in servers))
    finally:
        for t in tasks:
            t.cancel()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8770, help="control panel port")
    ap.add_argument("--public-port", type=int, default=8771, help="friends' call page port (tunnel this)")
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()
    url = f"http://127.0.0.1:{args.port}"
    log.info("PhoneLink Studio: control panel %s, call page http://127.0.0.1:%d", url, args.public_port)
    if not args.no_browser:
        threading.Timer(1.5, lambda: webbrowser.open(url)).start()
    asyncio.run(serve(args.port, args.public_port))


if __name__ == "__main__":
    main()
