# wifi-tui — By HIRECHE BAGHDAD BELKHEIR

![Terminal Based Interface](img/showcase.png)

An interactive ncurses Wi-Fi controller for the terminal. It scans nearby
access points, connects to open and secured networks, supports hidden and
custom SSIDs, and displays the current IPv4 address, netmask, gateway and DNS
servers.

There are two builds. They present the same interface and the same key
bindings; they differ only in what they drive underneath.

| | Linux build | FreeBSD build |
| --- | --- | --- |
| Source | [`wifi_tui.c`](wifi_tui.c) | [`FreeBSD/wifi_tui.c`](FreeBSD/wifi_tui.c) |
| Where to build | repository root | [`FreeBSD/`](FreeBSD/) |
| Backend | NetworkManager via `nmcli` | `wpa_supplicant` via `wpa_cli`, plus `ifconfig` |
| Target | Debian 13 and other NetworkManager systems | FreeBSD 15.1 |
| Guide | this file | [`FreeBSD/README.md`](FreeBSD/README.md) |

Pick the directory that matches your system and run `make` there. Neither
build depends on the other, and neither needs anything outside its platform's
usual packages.

---

## Linux (NetworkManager)

Targets Debian 13 and any other system where NetworkManager manages the Wi-Fi
device.

### Requirements

```bash
sudo apt install build-essential libncurses-dev network-manager
```

NetworkManager must manage the Wi-Fi device. Verify with:

```bash
nmcli device status
```

### Build and run

```bash
make
./wifi-tui
```

### Install

```bash
sudo make install
```

This installs `/usr/local/bin/wifi-tui` and the manual page
`/usr/local/share/man/man1/wifi-tui.1`. `sudo make uninstall` removes both.
`PREFIX`, `BINDIR`, `MANDIR` and `DESTDIR` are honoured:

```bash
make PREFIX=/usr
make DESTDIR=/tmp/stage install
```

### Test

```bash
make test
```

Runs the parser/UI smoke test against a fake `nmcli`. It does not touch the
machine's network configuration.

### Notes

The program normally runs as your desktop user. NetworkManager and Polkit may
ask for authorization depending on system policy; do not run the TUI with
`sudo` solely to connect to Wi-Fi.

For previously saved profiles, NetworkManager reuses stored credentials. For a
new secured network, the TUI requests a password after the first activation
attempt. WPA Enterprise/802.1X profiles should be configured first with
NetworkManager (for example with `nmtui` or `nm-connection-editor`); they can
then be activated through the normal NetworkManager tools.

---

## FreeBSD (wpa_supplicant)

Targets FreeBSD 15.1 and uses only the base system — no ports or packages.

```bash
cd FreeBSD
make
./wifi-tui
sudo make install
```

The interface has to be configured before the TUI is useful. In `/etc/rc.conf`,
with `iwlwifi0` replaced by whatever `sysctl net.wlan.devices` reports:

```
wlans_iwlwifi0="wlan0"
ifconfig_wlan0="WPA SYNCDHCP"
```

and in `/etc/wpa_supplicant.conf`:

```
ctrl_interface=/var/run/wpa_supplicant
ctrl_interface_group=wheel
update_config=1
```

`ctrl_interface_group=wheel` is what lets you scan and connect without `sudo`;
`update_config=1` is what lets new networks be saved across reboots. The full
walkthrough, including read-only mode and troubleshooting, is in
[`FreeBSD/README.md`](FreeBSD/README.md).

---

## Controls

Identical on both platforms.

| Key | Action |
| --- | --- |
| `Up` / `Down` | Select a network |
| `Left` / `Right` | Focus the network or status panel |
| `Enter` | Connect to the selected network |
| `a` | Add/connect to a custom or hidden SSID |
| `r` | Rescan nearby networks |
| `d` | Disconnect the Wi-Fi interface |
| `Home` / `End`, `PgUp` / `PgDn` | Move through a long list |
| `q` | Quit |

The window needs at least 54 columns by 12 rows. Above 90 columns the network
list and status panel sit side by side; below that they stack.

## Documentation

- `man wifi-tui` after installing, or `man ./wifi-tui.1` from a source
  directory
- [`FreeBSD/README.md`](FreeBSD/README.md) — the FreeBSD build in detail

## Licence

See [LICENSE](LICENSE).
