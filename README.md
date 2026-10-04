# IRIX-UDP-Mouse

Connect PiKVM to IRIX workstations via UDP mouse forwarding for absolute positioning, with optional UDP keyboard forwarding.

This project patches the kvmd `otg` HID plugin to forward mouse events over UDP to a daemon running on the IRIX machine, which injects them via the XTest extension. Keyboard input goes through USB HID by default, and can also (or instead) be forwarded over UDP — see [Keyboard transport](#keyboard-transport).

For PS/2 SGI systems - a USB to PS/2 converter or a PiKVM Pico HID-PS2 bridge can be used

When `irix_host` is not set, the plugin behaves as a standard `otg` plugin with no changes to normal operation.

Note - right now this has to run after login. I haven't figured out a way to run it for the login screen - as XDM is very specific about running a single executable and not allowing any other X clients.

https://github.com/user-attachments/assets/904c69bb-c6a9-4fdc-bdd1-7fbee85c944d

---

## Repository layout

```
kvmd/plugins/hid/otg/__init__.py   patched otg plugin — copy to PiKVM
irix/mouse.c                       UDP daemon — compile and run on IRIX
irix/mouse                         Pre-compiled binary of the IRIX receiver (older build: mouse only, no UDP keyboard)
```

The pre-compiled binary predates UDP keyboard support. Rebuild from `mouse.c` if you want `irix_keyboard: udp` or `both`.

---

## IRIX setup

### Build

`mouse.c` is strict C89 and builds with either the native MIPSpro compiler or gcc:

```sh
cd irix
cc -o mouse mouse.c -lX11 -lXtst      # MIPSpro
gcc -o mouse mouse.c -lX11 -lXtst     # gcc
```

### Run

```sh
./mouse [-v] [-d :0]
```

| Flag | Description |
|------|-------------|
| `-v` | Verbose output |
| `-d DISPLAY` | X display to use (default: `:0`) |

To start automatically at login, add to `~/.sgisession`.

---

## PiKVM setup

### 1. Switch PiKVM to read-write 
```sh
rw
```

### 2. Find the plugin path

```sh
python3 -c 'import kvmd.plugins.hid.otg as m; print(m.__file__)'
```

The path includes the Python version (e.g. `/usr/lib/python3.14/...`), which changes when PiKVM updates Python.

### 3. Back up the original (replace correct version number)

```sh
cp /usr/lib/python3.XX/site-packages/kvmd/plugins/hid/otg/__init__.py \
   /usr/lib/python3.XX/site-packages/kvmd/plugins/hid/otg/__init__.py.orig
```

### 4. Install the patched plugin (replace correct version number)

```sh
scp kvmd/plugins/hid/otg/__init__.py \
    root@pikvm:/usr/lib/python3.XX/site-packages/kvmd/plugins/hid/otg/__init__.py
```

### 5. Configure `/etc/kvmd/override.yaml`

```yaml
kvmd:
  hid:
    type: otg
    irix_host: "192.168.1.x"    # IP of your IRIX workstation
    irix_port: 5005              # must match the port the daemon listens on
    irix_screen_width: 1920      # IRIX display width in pixels
    irix_screen_height: 1200     # IRIX display height in pixels
    irix_keyboard: usb           # usb | udp | both — see "Keyboard transport"
```

### 6. Restart kvmd

```sh
systemctl restart kvmd
journalctl -u kvmd --since "-1 min" --no-pager | grep IRIX
```

You should see `IRIX: forwarding mouse to <host>:<port> (WxH), keyboard transport: <mode>`. If there is no `IRIX:` line, kvmd is not running the patched file.

**kvmd updates overwrite the patched plugin.** After every `pacman -Syu`, repeat steps 1–4 and 6.

## Keyboard transport

`irix_keyboard` controls where keystrokes go when `irix_host` is set (without `irix_host` everything goes to USB as normal):

| Value | USB HID keyboard | UDP to IRIX daemon | Use when |
|-------|------------------|--------------------|----------|
| `usb` (default) | yes | no | The USB keyboard works inside IRIX |
| `udp` | no | yes | You only need the keyboard in the logged-in X session |
| `both` | yes | yes | The USB keyboard works at the PROM but not inside IRIX |

Like the mouse, UDP keys only work while the daemon is running in the X session (after login). The PROM, text console and XDM login screen still need the USB keyboard, so `both` is the usual choice when the USB keyboard misbehaves inside IRIX. If the USB keyboard also works inside IRIX, `both` will type every key twice — use `usb` or `udp` instead.

The daemon maps Linux key codes to US-layout X keysyms and injects them with XTest, so Shift/Ctrl/Alt combinations work. Media/volume/power keys are not mapped. When the PiKVM web session connects or disconnects, the plugin sends `RESET` so the daemon releases any keys or mouse buttons still held down.

## PiKVM kernel 6.12+ and the IRIX USB keyboard

PiKVM images with Linux 6.12 or newer changed the USB HID gadget driver in two ways that affect IRIX:

1. **GET_REPORT delay.** The kernel holds HID GET_REPORT requests for up to 2.5 s waiting for userspace. IRIX treats this as an error and logs `WARNING: disabling kbd after 5 consecutive errors` in `/var/adm/SYSLOG`. The patched plugin works around this at startup by presetting an immediate empty reply (logged as `IRIX: preset immediate GET_REPORT reply on ...`).
2. **Zero-length packets (suspected).** The kernel now follows every full-size HID report with a zero-length packet. Even with the GET_REPORT workaround, IRIX accepts the USB keyboard but ignores its keystrokes (the PROM is unaffected); the extra packet is the likely cause but this hasn't been confirmed. Use `irix_keyboard: both` to type in IRIX over UDP while keeping the USB keyboard for the PROM.

## UDP message format

The plugin sends plain-text UDP packets.

| Event | Format | Example |
|-------|--------|---------|
| Mouse move | `x_y` | `640_512` |
| Button | `name,state` | `left,True` / `right,False` |
| Scroll | `WHEEL_delta` | `WHEEL_3` / `WHEEL_-1` |
| Key | `KEY_code,state` | `KEY_30,True` / `KEY_30,False` |
| Release all | `RESET` | `RESET` |

Button names: `left`, `right`, `middle`, `up`, `down`

Key codes are Linux evdev codes (`KEY_A` = 30, `KEY_LEFTSHIFT` = 42, `KEY_ENTER` = 28, ...).

To test from any machine on the network:

```sh
echo -n "640_512" | nc -u 192.168.1.x 5005
echo -n "KEY_30,True" | nc -u 192.168.1.x 5005; echo -n "KEY_30,False" | nc -u 192.168.1.x 5005   # types "a"
```
