# IRIX-UDP-Mouse

Connect PiKVM to IRIX workstations via UDP mouse forwarding for absolute positioning.

This project patches the kvmd `otg` HID plugin to forward mouse events over UDP to a daemon running on the IRIX machine, which injects them via the XTest extension. Keyboard input continues to work normally through USB HID.

For PS/2 SGI systems - a USB to PS/2 converter or a PiKVM Pico HID-PS2 bridge can be used

When `irix_host` is not set, the plugin behaves as a standard `otg` plugin with no changes to normal operation.


https://github.com/user-attachments/assets/904c69bb-c6a9-4fdc-bdd1-7fbee85c944d

---

## Repository layout

```
kvmd/plugins/hid/otg/__init__.py   patched otg plugin — copy to PiKVM
irix/mouse.c                       UDP daemon — compile and run on IRIX
irix/mouse                         Pre-compiled binary of the IRIX reciever
```

---

## IRIX setup

### Build

```sh
cd irix
gcc -o mouse mouse.c -lX11 -lXtst
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

Switch PiKVM to read-write and find the installed version

```sh
find /usr/lib -path "*/kvmd/plugins/hid/otg/__init__.py" 2>/dev/null
```

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
```

### 6. Restart kvmd

```sh
systemctl restart kvmd
```

## UDP message format

The plugin sends plain-text UDP packets.

| Event | Format | Example |
|-------|--------|---------|
| Mouse move | `x_y` | `640_512` |
| Button | `name,state` | `left,True` / `right,False` |
| Scroll | `WHEEL_delta` | `WHEEL_3` / `WHEEL_-1` |

Button names: `left`, `right`, `middle`, `up`, `down`

To test from any machine on the network:

```sh
echo -n "640_512" | nc -u 192.168.1.x 5005
```
