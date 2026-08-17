# wifi-tui for FreeBSD

An interactive ncurses Wi-Fi controller for FreeBSD, built and tested against
**FreeBSD 15.1**. It scans nearby access points, connects to open, WPA2-PSK,
WPA3-SAE and WEP networks, supports hidden SSIDs, and shows the current IPv4
address, netmask, gateway and DNS servers.

This is the FreeBSD counterpart of the Linux build in the parent directory.
The user interface and key bindings are identical; only the plumbing
underneath differs.

## Why a separate source file

The Linux build talks to NetworkManager through `nmcli`. FreeBSD has no
NetworkManager in the base system, so this port drives the base tools directly:

| Job | Linux build | FreeBSD build |
| --- | --- | --- |
| Interface discovery | `nmcli device status` | `ifconfig -g wlan`, then `ifconfig -l` |
| Scanning | `nmcli device wifi list` | `wpa_cli scan` + `scan_results` |
| Connecting | `nmcli device wifi connect` | `wpa_cli add_network` / `select_network` |
| Saved profiles | NetworkManager connections | `wpa_supplicant.conf` networks |
| IPv4 address | `nmcli device show` | `ifconfig <iface>` |
| Gateway | `nmcli device show` | `netstat -rn -f inet` |
| DNS | `nmcli device show` | `/etc/resolv.conf` |
| DHCP | NetworkManager | `devd`, or `dhclient` as a fallback |

Everything it needs is in the FreeBSD base system. There are no ports or
packages to install.

## Requirements

FreeBSD 15.1 base system. Specifically:

- `cc` and `make` — base
- `libncursesw` and `ncurses.h` — base
- `wpa_supplicant`, `wpa_cli`, `ifconfig`, `netstat`, `dhclient` — base

If you would rather build against `devel/ncurses` from ports:

```bash
make CPPFLAGS=-I/usr/local/include/ncurses LDFLAGS=-L/usr/local/lib
```

## Set up the interface first

`wifi-tui` is a front end, not a replacement for configuring the interface.
Find your wireless device and create a `wlan` interface for it in
`/etc/rc.conf`, replacing `iwlwifi0` with what `sysctl net.wlan.devices`
reports:

```bash
sysctl net.wlan.devices
```

```
wlans_iwlwifi0="wlan0"
ifconfig_wlan0="WPA SYNCDHCP"
```

Then make the control socket usable, in `/etc/wpa_supplicant.conf`:

```
ctrl_interface=/var/run/wpa_supplicant
ctrl_interface_group=wheel
update_config=1
```

- `ctrl_interface_group=wheel` lets accounts in `wheel` scan and connect
  without `sudo`.
- `update_config=1` lets `wifi-tui` save new networks so they survive a
  reboot. Without it a connection works but is forgotten on restart, and
  `wifi-tui` says so.

Bring it up:

```bash
service netif restart wlan0
```

Verify:

```bash
wpa_cli -i wlan0 status
```

## Build and run

```bash
make
./wifi-tui
```

Install to `/usr/local`:

```bash
sudo make install
```

That puts the program in `/usr/local/bin/wifi-tui` and the manual page in
`/usr/local/share/man/man1/wifi-tui.1`. `make uninstall` removes both.

The Makefile works with FreeBSD's `make` and with GNU `make`. It honours
`PREFIX`, `BINDIR`, `MANDIR`, `DESTDIR`, `CC`, `CFLAGS`, `CPPFLAGS`,
`LDFLAGS` and `LDLIBS`:

```bash
make PREFIX=/usr
make DESTDIR=/tmp/stage install
```

## Test

```bash
make test
```

The smoke test runs the real binary inside a pty with fake `ifconfig`,
`wpa_cli` and `netstat` programs on `PATH` and a fixture `resolv.conf`, then
checks what reached the screen. It never touches your network configuration.
It takes about four seconds, most of which is the scan settle delay.

## Controls

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

## How connecting works

If the SSID already has a profile in `wpa_supplicant.conf`, `wifi-tui`
activates it and never asks for a passphrase. Otherwise it prompts, creates
the network with `add_network`, and selects it.

- Passphrases go to `wpa_cli` over **standard input**, not on the command
  line, so they never show up in `ps` output.
- A 64-digit hex string is used as a raw PSK; a 10- or 26-digit hex string as
  a raw WEP key. Anything else is treated as a passphrase.
- `select_network` disables every other profile, so if the association fails
  `wifi-tui` runs `enable_network all` and `reconnect` to put the supplicant
  back into normal roaming. A wrong password will not leave you stranded.
- After associating, it waits for an IPv4 address. `devd` normally starts
  `dhclient` for you. If no lease appears and you are root, it runs
  `dhclient -b`; if you are not root, it tells you the command to run.

WPA Enterprise (802.1X) networks are listed and flagged but cannot be
configured here — they need an `eap`/`identity`/`password` block in
`wpa_supplicant.conf`. Once that block exists, `wifi-tui` activates the
profile like any other.

## Read-only mode

If the `wpa_supplicant` control socket cannot be reached, `wifi-tui` does not
quit. It falls back to listing the `net80211` scan cache through
`ifconfig <iface> list scan`, shows `ifconfig (read-only)` in the header, and
refuses connect and disconnect.

That usually means one of:

- `wpa_supplicant` is not running — check `ifconfig_wlan0="WPA SYNCDHCP"` in
  `rc.conf` and run `service netif restart wlan0`
- your account is not in the group named by `ctrl_interface_group`
- the control socket is somewhere other than `/var/run/wpa_supplicant`; point
  at it with `WIFI_TUI_WPA_CTRL`

## Environment

Both are ignored when the process is set-user-ID or set-group-ID.

| Variable | Meaning |
| --- | --- |
| `WIFI_TUI_RESOLV_CONF` | Resolver file to read DNS servers from. Default `/etc/resolv.conf`. |
| `WIFI_TUI_WPA_CTRL` | Control socket directory, passed to `wpa_cli -p`. Default is whatever `wpa_cli` picks, normally `/var/run/wpa_supplicant`. |

## Troubleshooting

**"No 802.11 interface found"** — no `wlan` interface exists yet. Create one:

```bash
sudo ifconfig wlan create wlandev iwlwifi0
```

then make it permanent in `rc.conf` as shown above.

**Scanning returns nothing** — the interface may be down. `ifconfig wlan0 up`,
or `service netif restart wlan0`.

**"not saved: set update_config=1"** — the connection is live but only in the
running supplicant. Add `update_config=1` to `/etc/wpa_supplicant.conf`.

**Connected but no IPv4** — `devd` did not start DHCP. Run
`sudo dhclient wlan0`, and check that `ifconfig_wlan0` in `rc.conf` includes
`SYNCDHCP` or `DHCP`.

## Portability notes

The source is C11 and builds clean under `-Wall -Wextra -Wpedantic`. Two
FreeBSD-specific points are worth knowing if you port it further:

- `_POSIX_C_SOURCE` is deliberately **not** defined. FreeBSD's
  `<sys/cdefs.h>` already exposes POSIX 2008 by default, and defining it would
  set `__BSD_VISIBLE` to 0 and hide `issetugid(2)` and `explicit_bzero(3)`.
- The Makefile avoids `$<` and `$^`, which BSD make only defines inside suffix
  rules, and avoids `.if`/`ifeq`, which bmake and GNU make spell differently.

## See also

`wifi-tui(1)`, `wpa_supplicant.conf(5)`, `wpa_cli(8)`, `ifconfig(8)`,
`rc.conf(5)`, `net80211(4)`.
