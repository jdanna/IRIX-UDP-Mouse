# IRIX-UDP-Mouse

Use a [PiKVM](https://pikvm.org/) with an SGI IRIX workstation, with **absolute mouse positioning** and an optional **network keyboard**.

IRIX has no support for USB absolute-pointer devices (tablets), so the PiKVM web UI's mouse never lines up with the IRIX cursor. This project fixes that by sending mouse events (and optionally keystrokes) over the network instead:

- A patched copy of the kvmd `otg` HID plugin runs on the PiKVM and sends mouse events, and optionally keystrokes, as small UDP messages.
- A small daemon (`mouse`) runs on the IRIX machine, receives them, and injects them into the X server with the XTest extension.

```
 Browser ──► PiKVM (kvmd + patched otg plugin) ──UDP :5005──► IRIX (mouse daemon) ──XTest──► X server
                       │
                       └──USB HID keyboard──────────────────► IRIX (PROM, text console)
```

When `irix_host` is not set, the plugin behaves exactly like the stock kvmd `otg` plugin.

https://github.com/user-attachments/assets/904c69bb-c6a9-4fdc-bdd1-7fbee85c944d

### Security warning!
This is a total hack and leaves XDM exposed to external clients, and accepts mouse and keyboard input from the network. You shouldn't ever be exposing an IRIX machine to the public internet anyway, but please keep this on internal networks only.

### Limitations

- **Only works while X is running.** The daemon is an X client, so the PROM and the text console still need the USB keyboard. It does work at the graphical login screen if you start it from XDM; see [Starting at the login screen (XDM)](#starting-at-the-login-screen-xdm).
- **The mouse needs the PiKVM in absolute mouse mode.** Only absolute moves are forwarded; relative mouse mode still goes out over USB.
- **Keystrokes use a US layout.** Media, volume and power keys are not forwarded.
- **kvmd updates overwrite the plugin.** Reinstall it after every PiKVM update

### Connecting the keyboard

- **SGI systems with USB:** connect the PiKVM's USB OTG port directly.
- **PS/2 SGI systems:** use a USB to PS/2 converter or a PiKVM Pico HID-PS2 bridge.

---

## Repository layout

```
kvmd/plugins/hid/otg/__init__.py   Patched kvmd otg HID plugin. Copy it onto the PiKVM.
irix/mouse.c                       IRIX daemon source (strict C89, builds with MIPSpro cc or gcc).
```

No prebuilt binary is included; build the daemon on the IRIX machine as described below.

---

## IRIX setup

### Build

```sh
cd irix

# MIPSpro, default 32-bit (n32) ABI
cc -o mouse mouse.c -lXtst -lXext -lX11

# MIPSpro, 64-bit
cc -64 -o mouse mouse.c -lXtst -lXext -lX11

# gcc
gcc -o mouse mouse.c -lXtst -lXext -lX11
```


### Run

```sh
./mouse [-v] [-d DISPLAY]
```

| Flag | Description |
|------|-------------|
| `-v` | Verbose: prints every received message and what was done with it |
| `-d DISPLAY` | X display to use (default `:0`) |

The daemon listens on UDP port **5005** on all interfaces. On startup it checks that the X server supports XTest and exits with an error if it doesn't.

### Starting at the login screen (XDM)

To have the mouse (and UDP keyboard) work at the graphical login screen, and from boot onward without logging in first, start the daemon from XDM. Do all of this as root.

**1. Install the binary:**

```sh
cp mouse /usr/bin/mouse
chmod 755 /usr/bin/mouse
```

**2. Start it from `Xsetup_0`.**  Add this line to `/var/X11/xdm/Xsetup_0` 

```sh
/usr/bin/mouse -d :0&
```

**3. Turn off XDM's X authorization.** By default XDM only allows the login screen to autohrize. In `/var/X11/xdm/xdm-config`, add or change this line:

```
DisplayManager*authorize:               off
```

**4. Restart XDM ** :

```sh
/etc/init.d/xdm stop
/etc/init.d/xdm start
```

---

## PiKVM setup

On the PiKVM as root:

### 1. Make the filesystem writable

```sh
rw
```

### 2. Find the installed plugin

```sh
python3 -c 'import kvmd.plugins.hid.otg as m; print(m.__file__)'
```

This prints something like `/usr/lib/python3.14/site-packages/kvmd/plugins/hid/otg/__init__.py`. The Python version in that path changes when PiKVM upgrades Python, so look it up each time rather than reusing an old path.

### 3. Back up the original

```sh
P=$(python3 -c 'import kvmd.plugins.hid.otg as m; print(m.__file__)')
cp -p "$P" "$P.orig"
```

### 4. Install the patched plugin

From the machine with this repository, using the path from step 2:

```sh
scp kvmd/plugins/hid/otg/__init__.py root@pikvm:/usr/lib/python3.XX/site-packages/kvmd/plugins/hid/otg/__init__.py
```


### 5. Configure `/etc/kvmd/override.yaml`

Add the `irix_*` keys under `kvmd:` → `hid:`:

```yaml
kvmd:
    hid:
        type: otg
        irix_host: "192.168.1.x"     # IP of the IRIX workstation; empty = stock behavior
        irix_port: 5005              # must match the daemon (fixed at 5005)
        irix_screen_width: 1920      # IRIX display width in pixels
        irix_screen_height: 1200     # IRIX display height in pixels
        irix_keyboard: both          # usb | udp | both, see "Keyboard transport"
```

### 6. Restart kvmd and check

```sh
systemctl restart kvmd
journalctl -u kvmd --since "-1 min" --no-pager | grep IRIX
ro
```

You should see:

```
IRIX: forwarding mouse to 192.168.1.x:5005 (1920x1200), keyboard transport: both
```

If there's no `IRIX:` line, kvmd isn't running the patched file; recheck the path from step 2.

In the PiKVM web UI, set the mouse mode to **absolute** (`usb`).

---

## Configuration reference

| Key | Default | Description |
|-----|---------|-------------|
| `irix_host` | *(empty)* | IP or hostname of the IRIX machine. Empty turns off all IRIX forwarding (stock behavior). |
| `irix_port` | `5005` | UDP port to send to. The daemon always listens on 5005. |
| `irix_screen_width` | `1920` | IRIX screen width in pixels, used to scale mouse positions. |
| `irix_screen_height` | `1200` | IRIX screen height in pixels. |
| `irix_keyboard` | `usb` | Where keystrokes go: `usb`, `udp` or `both`. |

### Mouse position mapping

kvmd reports absolute positions in the range `-32768` to `32768`. The plugin scales them to pixels:

```
pixel_x = (x + 32768) / 65536 * (irix_screen_width  - 1)
pixel_y = (y + 32768) / 65536 * (irix_screen_height - 1)
```

If the cursor doesn't reach the screen edges, or overshoots them, the width and height don't match the IRIX resolution.

---

## Keyboard transport

`irix_keyboard` decides where keystrokes go when `irix_host` is set. Without `irix_host`, everything goes to USB as normal.

| Value | USB keyboard | UDP to the daemon | Use when |
|-------|--------------|-------------------|----------|
| `usb` (default) | yes | no | The USB keyboard works everywhere, including inside IRIX |
| `udp` | no | yes | You only need the keyboard in the logged-in X session |
| `both` | yes | yes | The USB keyboard works at the PROM but not inside IRIX |

- **UDP keys only work while the daemon is running.** That covers the desktop, and the login screen too if you start the daemon from XDM. The PROM and the text console always need the USB keyboard, which is why `both` keeps USB going too.
- **`both` can type every key twice.** If the USB keyboard also works inside IRIX, every keystroke arrives over USB and over UDP. Switch to `usb` or `udp` if that happens.
- **Modifier keys work.** Shift, Ctrl and Alt are sent as separate key presses and tracked by the X server, so Shift+A gives `A` and Ctrl+C works.
- **Missing keys are skipped.** If a key isn't in the IRIX keymap, the daemon tries a fallback (Right Shift → Left Shift, keypad digits → keypad navigation keys, and so on). If there's no fallback, it skips the key and logs it with `-v`.
- **Nothing stays held down.** When a PiKVM web session connects or disconnects, the plugin sends `RESET`, and the daemon releases any keys or mouse buttons it's still holding.

---

## UDP message format

Plain-text UDP datagrams, one event per packet, sent to port 5005.

| Event | Format | Example |
|-------|--------|---------|
| Mouse move | `x_y` | `640_512` |
| Mouse button | `name,state` | `left,True` / `left,False` |
| Scroll wheel | `WHEEL_delta` | `WHEEL_5` / `WHEEL_-5` |
| Key | `KEY_code,state` | `KEY_30,True` / `KEY_30,False` |
| Release all | `RESET` | `RESET` |

- **Mouse buttons:** `left`, `middle`, `right`. `up` and `down` (side buttons) are sent but ignored by the daemon.
- **Scrolling:** turned into Page Up / Page Down, one press per 5 units of scroll.
- **Key codes:** Linux evdev codes, for example `KEY_A` = 30, `KEY_ENTER` = 28 and `KEY_LEFTSHIFT` = 42 (full list in `linux/input-event-codes.h`). `True` means pressed, `False` released.

Manual tests from any machine on the network:

```sh
echo -n "640_512"      | nc -u <irix-ip> 5005     # move the cursor
echo -n "left,True"    | nc -u <irix-ip> 5005     # press left button
echo -n "left,False"   | nc -u <irix-ip> 5005     # release it
echo -n "KEY_30,True"  | nc -u <irix-ip> 5005     # press "a"
echo -n "KEY_30,False" | nc -u <irix-ip> 5005     # release "a"
echo -n "RESET"        | nc -u <irix-ip> 5005     # release everything
```

---

## Troubleshooting

### PiKVM side

- **No `IRIX:` line in the kvmd log.** kvmd is loading a different file. Rerun step 2 and copy the plugin to that exact path.
- **`Unknown plugin 'hid/otg'` in the kvmd log.** The plugin failed to load. The real Python error is printed just above that line.
- **An `Invalid value` error mentioning `irix_keyboard`.** It must be `usb`, `udp` or `both`.
- **Mouse doesn't move.** Check that the web UI's mouse mode is absolute (`usb`). Then check that packets are leaving the PiKVM while you move the mouse:
  ```sh
  pacman -Sy tcpdump        # once, needs rw
  tcpdump -ni any udp port 5005
  ```
  - **Packets leave:** the problem is on the IRIX side.
  - **No packets:** watch `journalctl -u kvmd -f` for errors while you move the mouse.

### IRIX side

- **`Bind failed: Address already in use`.** Another copy of the daemon is running; check with `ps -ef | grep mouse`.
- **`Unable to open X display :0`.** Run the daemon from inside your desktop session, or set `DISPLAY` first. This also happens when you're logged in over telnet or SSH. If you start it from `Xsetup_0`, this error usually means XDM's authorization is still on; check `DisplayManager*authorize: off` in `xdm-config` and restart graphics.
- **Nothing works at the login screen.** Check that `/usr/bin/mouse -d :0&` is in `/var/X11/xdm/Xsetup_0` before any `exit`, and that the daemon is running (`ps -ef | grep mouse`) while the login screen is up.
- **Nothing arrives at all.** Try `echo -n "640_512" | nc -u <irix-ip> 5005` from the PiKVM. If that does nothing, check the network connectivity
- **Keys typed twice.** You're using `irix_keyboard: both` and the USB keyboard also works inside IRIX. Switch to `usb` or `udp`.

