# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Bridges a [PiKVM](https://pikvm.org/) to an SGI IRIX workstation. IRIX can't use USB absolute-pointer devices, so mouse events (and optionally keystrokes) are sent over UDP to a daemon on the IRIX machine, which injects them into X with XTest.

Two components:

- **`kvmd/plugins/hid/otg/__init__.py`**: a patched copy of kvmd's stock `otg` HID plugin. It's installed on the PiKVM by overwriting the stock file at `python3 -c 'import kvmd.plugins.hid.otg as m; print(m.__file__)'`. When `irix_host` is set, it forwards mouse events (and keys, depending on `irix_keyboard`) over UDP. Otherwise it behaves exactly like stock.
- **`irix/mouse.c`**: the IRIX daemon. It listens on UDP 5005, parses the text protocol, and injects events with `XTestFake*Event`.

`irix/mouse` is an old prebuilt binary (mouse only, no keyboard support). Don't treat it as built from the current source.

On IRIX the daemon is normally installed as `/usr/bin/mouse`. It's started at the XDM login screen by adding `/usr/bin/mouse -d :0&` to `/var/X11/xdm/Xsetup_0`, which needs `DisplayManager*authorize: off` in `/var/X11/xdm/xdm-config` so it can connect to the X server. It can also be started per user from `~/.sgisession`. The README documents both.

## The plugin (`kvmd/plugins/hid/otg/__init__.py`)

- **It must stay in sync with upstream kvmd.** It's the stock otg plugin plus IRIX additions. When kvmd changes the plugin API (as it did in 2026, when constructors switched to `__init__(self, c: Section)` and `get_plugin_options()` started calling `super()`), diff it against the current upstream file at `https://github.com/pikvm/kvmd/blob/master/kvmd/plugins/hid/otg/__init__.py` and carry the IRIX additions over. Keep everything that isn't IRIX-specific identical to upstream.
- **Config options** (under `kvmd: hid:` in `/etc/kvmd/override.yaml`): `irix_host`, `irix_port`, `irix_screen_width`, `irix_screen_height`, `irix_keyboard` (`usb` | `udp` | `both`, default `usb`, validated by `_valid_irix_keyboard`). kvmd silently ignores unknown config keys, so a stock plugin with `irix_*` keys still runs normally.
- **Methods it overrides**:
  - `_send_mouse_move_event` scales kvmd's `-32768..32768` range to pixels using the configured screen size.
  - `_send_mouse_button_event` maps evdev button codes to names via `_BUTTON_CODES`.
  - `_send_mouse_wheel_event` forwards the vertical delta only.
  - `_send_key_event` sends to UDP when `irix_keyboard != "usb"`, and to USB when `irix_keyboard != "udp"` or when `irix_host` is empty.
  - `_clear_events` and `reset` also send `RESET` over UDP.
  - Relative mouse events always go to USB.
- **Logging:** `sysprep()` logs `IRIX: forwarding mouse to host:port (WxH), keyboard transport: mode`. Troubleshooting relies on that line to confirm the patched file is the one loaded; keep it.
- `set_params` accepts `irix_*` kwargs, but kvmd's HTTP API never passes them, so they're effectively unused.
- UDP sends use a fresh socket per message. Send errors are logged as warnings and never raised.

## The daemon (`irix/mouse.c`)

- **It must compile with the native MIPSpro `cc`**, which is strict C89:
  - Use `/* */` comments only, never `//`.
  - Put all declarations at the start of a block.
  - Don't use C99 features: no designated initializers, no `for (int i...)`, no `stdbool`/`stdint`.
  - Avoid `socklen_t`; the daemon uses `recv()`, so it never needs the sender's address.
  - Guard non-core keysyms with `#ifdef XK_...`. IRIX's X11R6 `keysymdef.h` has no XF86 keysyms.
- **Build commands.** Library order matters, because the 64-bit `libXtst` is a static archive:
  ```sh
  cc     -o mouse mouse.c -lXtst -lXext -lX11   # MIPSpro n32
  cc -64 -o mouse mouse.c -lXtst -lXext -lX11   # MIPSpro 64-bit
  gcc    -o mouse mouse.c -lXtst -lXext -lX11
  ```
- **Run:** `./mouse [-v] [-d DISPLAY]`. It checks for XTest at startup and line-buffers stdout so `-v` output shows up when redirected.
- **Message dispatch order** in the main loop: `KEY_` → `RESET` → `WHEEL_` → anything containing a comma (button) → `%d_%d` (move). `KEY_` must be checked before the comma case, because key messages also contain a comma.
- **`KEY_MAP`** maps Linux evdev key codes (what kvmd's `WEB_TO_EVDEV` produces) to US-layout X keysyms, each with an optional fallback keysym. Only unshifted keysyms are listed; modifiers arrive as separate key events. Media, volume, power and `KEY_RO` are intentionally unmapped. When kvmd adds keys, regenerate the list from `kvmd/keyboard/mappings.py`.
- **`keys_down[]` and `buttons_down[]`** track what the daemon has pressed, so `RESET` (`release_all`) releases only those.

## UDP protocol (port 5005, one plain-text event per datagram)

| Event | Format | Example |
|---|---|---|
| Mouse move | `X_Y` (pixels) | `640_480` |
| Button | `name,True\|False` (`left`, `middle`, `right`; `up`/`down` are ignored by the daemon) | `left,True` |
| Scroll | `WHEEL_delta` (one Page Up/Down per 5 units) | `WHEEL_-5` |
| Key | `KEY_<evdev code>,True\|False` | `KEY_30,True` |
| Release all | `RESET` | `RESET` |

Unknown messages are ignored (and logged with `-v`), so older and newer plugin and daemon versions can be mixed safely.

## Testing without the hardware

- **The plugin** needs Python 3.13+, because upstream kvmd uses PEP 696-style generics such as `Generator[None]`. Clone upstream kvmd, copy the plugin into `kvmd/plugins/hid/otg/`, and create a 3.14 venv with `evdev pyyaml aiohttp setproctitle` and similar dependencies. Build it with `make_config({}, {...}, Plugin.get_plugin_options())`, pass `noop: True`, replace `_Plugin__keyboard_proc` with a stub if you need to see the USB calls, and listen on UDP 5005 to check the output.
- **The daemon** runs on Linux too. Check C89 compliance with `gcc -std=c89 -pedantic-errors -Wall -Wextra -Wdeclaration-after-statement -D_POSIX_C_SOURCE=200112L`. For an end-to-end test, run it with `-d :99` against `Xvfb :99`, alongside a small X client that takes focus and prints the `KeyPress`/`ButtonPress`/`MotionNotify` events it receives.
- **A pitfall:** `pkill -f` / `pgrep -f` patterns can match the invoking shell's own command line. Kill test processes with `pgrep -x <name>`.

## Docs

`README.md` is the user-facing install and troubleshooting guide. Keep its config table, build commands and protocol table in sync with the code.
