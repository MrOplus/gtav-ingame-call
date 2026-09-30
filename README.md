# GTA V In-Game Call (PhoneLink)

Real phone calls and text messages with your friends **inside GTA V story mode**.

Friends open a link in their browser (phone or PC) and call you. Your in-game phone rings; you answer with **Y** and talk through your headset. You can also call them from the game (**F7**), and they can text your in-game phone.

```
friend's browser  ──WebRTC voice (direct)──►  PhoneLink Studio (your PC)  ──►  PhoneLink.asi  ──►  in-game phone
        └──── call page + signaling via Cloudflare Tunnel ────┘
```

| Part | What it is |
|---|---|
| `PhoneLink.asi` | Script Hook V plugin (C++). Rings the in-game phone, draws the call panel, plays call audio and captures your mic, and shows texts. Controlled through a local HTTP/WebSocket API. |
| **PhoneLink Studio** (`studio/`) | Small local app (Python). Control panel for you, and the call page for your friends, with WebRTC audio. |

## Features

- **Friends call you.** Your phone rings in-game with their name. **Y** answers, **N** declines or hangs up.
- **You call friends from the real in-game phone.** Online friends appear in the phone's **Contacts** app (Up arrow). Selecting one uses the phone's own call screen, and their browser rings. **F7** opens a fallback contacts menu.
- **Texts.** Friends' messages arrive as in-game phone notifications.
- **Busy and offline.** While you're on a call, friends see *Busy*. With the game closed, they see *Not in game*.
- **Invite link.** Only people with your code can call. Repeated wrong codes are rate-limited.
- **Nothing to install for callers.** A modern browser with a microphone is enough.

## Quick start

1. **Install Script Hook V** (`ScriptHookV.dll` + `dinput8.dll`) into your GTA V folder, from [dev-c.com](http://www.dev-c.com/gtav/scripthookv/). You need GTA V Legacy, single player.
2. **Download** `PhoneLink-<version>.zip` from [Releases](../../releases) and unzip it anywhere.
3. **Run** `studio\Start.bat`. The first run installs about 150 MB of Python packages, then the control panel opens at http://127.0.0.1:8770.
4. **Setup tab → Install / update plugin.** This copies `PhoneLink.asi` into your game folder. Close the game first.
5. **Put the call page online:** `cloudflared tunnel --url http://127.0.0.1:8771`. Paste the `https://….trycloudflare.com` address it prints into **Setup → Public address**.
6. **Send the invite link** from the **Calls** tab to your friends, and start GTA V story mode.

The full walkthrough is in **[docs/USER_GUIDE.md](docs/USER_GUIDE.md)**.

## Documentation

- [User guide](docs/USER_GUIDE.md): setup, playing, the tunnel, troubleshooting
- [API reference](docs/API.md): the plugin's HTTP/WebSocket API and Studio's signaling protocol
- [Development](docs/DEVELOPMENT.md): building, architecture, tests, CI and releases

## Notes

- **Story mode only.** Script Hook V refuses to run in GTA Online.
- **Game updates:** after a GTA V update, wait for a matching Script Hook V release. PhoneLink itself doesn't depend on the game version.
- **Headphones:** use them, otherwise your mic picks up the game and your friend's voice.
- **Privacy:** only the call page (port 8771) should be public. Never expose the control panel (port 8770).
