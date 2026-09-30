# User guide

## Requirements

- GTA V **Legacy** (Steam, Epic or Rockstar), single player
- [Script Hook V](http://www.dev-c.com/gtav/scripthookv/) matching your game version. You need `ScriptHookV.dll` and the ASI loader `dinput8.dll` in the game folder.
- Windows 10/11, a microphone, and headphones (strongly recommended)
- [cloudflared](https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/downloads/), to put your call page online: `winget install Cloudflare.cloudflared`

## 1. Install

1. Unzip `PhoneLink-<version>.zip` anywhere, e.g. `C:\PhoneLink`.
2. Double-click `studio\Start.bat`. On the first run it installs Python 3.11 and the packages Studio needs (about 150 MB). This uses [uv](https://docs.astral.sh/uv/), which it installs if missing. When it's done, the control panel opens in your browser at **http://127.0.0.1:8770**.
3. On the **Setup** tab, check the game folder. It's detected automatically for Steam, Epic and Rockstar installs.
4. Press **Install / update plugin**. This copies `PhoneLink.asi` and `PhoneLink.ini` into the game folder. The game must be closed, and your existing `PhoneLink.ini` is never overwritten.

To install by hand instead, copy `studio\plugin\PhoneLink.asi` and `PhoneLink.ini` into the GTA V folder, next to `ScriptHookV.dll`.

## 2. Put your call page online

Friends reach you through a Cloudflare Tunnel that points at Studio's **call page on port 8771**. Only that port goes online.

- **Quick tunnel** (random address, which changes every time it starts):
  ```
  cloudflared tunnel --url http://127.0.0.1:8771
  ```
  Copy the `https://….trycloudflare.com` address it prints. A brand-new address can take up to a minute before it works.
- **Permanent address** on your own domain (managed by Cloudflare):
  ```
  cloudflared tunnel login
  cloudflared tunnel create phonelink
  cloudflared tunnel route dns phonelink call.example.com
  cloudflared tunnel run --url http://127.0.0.1:8771 phonelink
  ```

Paste the public address into **Setup → Public address** and save. Your invite link is shown on the **Calls** tab.

> ⚠️ Never tunnel port **8770**. That's your private control panel, and it can install files into your game folder.

## 3. Play

1. Start GTA V story mode. The **Game** chip in the control panel turns green, and friends see *In game — available*.
2. On the **Calls** tab, set **Your name**. It's shown on the call page, and to friends when you call them.
3. Press **Ring me** to check that the in-game phone works.
4. Send your invite link to friends.

**In game:**

| Key | Action |
|---|---|
| **Y** | Answer an incoming call |
| **N** | Decline, hang up, or cancel an outgoing call |
| **Up arrow** → Contacts | Your online friends are listed with the game's contacts. Select one to call them; the phone shows its own *Dialing…* / *Connected* screen. The phone's back button hangs up. |
| **F7** | Open/close PhoneLink's own contacts menu (fallback) |
| **Up/Down**, **Enter** | Pick a friend in the contacts menu and call them |
| **Backspace** | Close the contacts menu |

You can change these keys in `PhoneLink.ini`, under `[Call]`, using Windows virtual-key codes.

**On your friend's side** (the invite link):

- **Call** rings you in-game. They hear ringback until you answer.
- **Text box** sends a message to your in-game phone.
- **Incoming calls:** when you call them from the game, their page rings with **Accept / Decline**. They must have the page open (a background tab is fine) to show up in your F7 contacts.
- **Name:** entered once and remembered, together with the invite code.

## Settings

**`PhoneLink.ini`** (in the game folder):

| Section | Key | Default | Meaning |
|---|---|---|---|
| Server | `Host` / `HttpPort` / `WsPort` | 127.0.0.1 / 8765 / 8766 | Local API address. Keep it on 127.0.0.1. |
| Server | `Token` | *(empty)* | Require a token for the API. Also set it under Studio's **Setup → Plugin connection**. |
| Call | `RingTimeoutSec` | 25 | Seconds before an unanswered incoming call counts as missed |
| Call | `AnswerKey` / `HangupKey` / `ContactsKey` | 0x59 (Y) / 0x4E (N) / 0x76 (F7) | Keys |
| Call | `PhoneAnimation` | 1 | Put the phone to your character's ear during calls |
| Call | `NativePhone` | 1 | Show friends in the game phone's Contacts app and use its call screen. Set 0 if it conflicts with a mission or another phone mod. |
| Audio | `Volume` | 1.0 | Call volume |
| Log | `Level` | info | `debug` / `info` / `warn` / `error`. The log file is `PhoneLink.log` in the game folder. |

**Studio** keeps its settings (name, invite code, tunnel address, TURN relay) in `studio\settings.json`. Change them in the control panel. **New invite code** invalidates old links.

## Troubleshooting

| Problem | Fix |
|---|---|
| Control panel says *Game: not running* | Make sure `PhoneLink.asi` is in the game folder and you're in story mode. Check `PhoneLink.log` there; it should mention `API: HTTP listening`. |
| The friend's page says *Not in game* | Same as above. Also check that Studio is running. |
| The friend can't open the link | The tunnel isn't running, or the quick-tunnel address changed. Restart it and update **Setup → Public address**. |
| Call connects but there's no audio | The friend's network blocks direct connections. Add a TURN relay under **Setup → Relay server**. |
| Friend hears echo | Use headphones. |
| Launcher opens, then nothing happens | An old `GTA5.exe` is still stuck from the last session. End it in Task Manager (Details tab). If it can't be ended and you use **Cloudflare WARP**, restart the *Cloudflare One Client* service or reboot. |
| Friends missing from the phone's Contacts | They need your call page open *and* a name entered. They're added below the game's own contacts, so scroll down. |
| Phone behaves oddly during a mission | Set `NativePhone=0` in `PhoneLink.ini` and use **F7** instead. |
| GTA hangs when quitting | Usually another mod crashing during shutdown. It has been seen with *Enhanced Native Trainer*. Update or remove that mod. |

Logs:
- `PhoneLink.log` in the game folder (the previous session's log is kept as `PhoneLink.prev.log`)
- `studio\studio.log`
- the control panel's **Log** tab
