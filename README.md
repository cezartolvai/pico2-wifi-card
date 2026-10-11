# pico2-wifi-card

A Raspberry Pi **Pico 2 W** that shows up to your computer as a USB network
adapter (CDC-NCM) and puts the host directly on your Wi-Fi network: one bridge,
one MAC, one address, no NAT.

```
USB host  <->  Pico 2 W  <->  Wi-Fi
your NIC     L2 bridge      your home network
             one MAC        one lease, from your router
```

**Target: a network card, not a router.** Recorded in
`.clinerules/00-requirements-baseline.md` ("Decizii consemnate", 2026-10-09).

**`src/` now holds both data paths, and `src/config.h` picks one at build time.**
`BRIDGE_L2_BRIDGE` (in `src/config.h`, default `0`) picks which data path is compiled:

- `BRIDGE_L2_BRIDGE = 0` — the default, and what `build/` and the flashed board contain today:
  an L3 router. One lwIP stack holding two interfaces, `IP_FORWARD`, a TinyUSB DHCP server and
  source NAT.
- `BRIDGE_L2_BRIDGE = 1` — the transparent bridge the requirements ask for: Ethernet frames
  forwarded between the two ports, the host adopting the station's MAC, the station's own DHCP
  client stopped, no NAT, no private subnet, no Pico-side lease. **Built and checked at the
  symbol level; not yet flashed or hardware-tested.**

| netif | `BRIDGE_L2_BRIDGE = 0` (today) | `BRIDGE_L2_BRIDGE = 1` |
|-------|--------------------------------|------------------------|
| `u1`  | TinyUSB CDC-NCM, `172.28.0.1` gateway, leases the host `172.28.0.10-17` | TinyUSB CDC-NCM, **no address at all**; the host takes its own lease from your router |
| `w0`  | CYW43 station, leased by your router, `output = bridge_nat_output` | CYW43 station, **no address of its own** (its own DHCP client is stopped) |

```sh
cmake -S . -B build-l2 -G Ninja -DPICO_SDK_PATH=$PWD/.pico-sdk -DBRIDGE_L2_BRIDGE=1
ninja -C build-l2
```


## Requirements reference vs. what is built today

`openrouter-banana2-rpi-pico.png` is the **requirements reference** for this product: it says
what the adapter must do. `src/` is today's implementation, and it does not yet meet most of
that reference.

| Requirement (reference image) | Today in `src/` |
|-------------------------------|-----------------|
| Transparent L2 bridging | in code, behind `-DBRIDGE_L2_BRIDGE=1`: `bridge_l2_forward()` hands each frame to the other port's `linkoutput`, and `IP_FORWARD` / both lwIP hooks are not defined in that build. The default build (`build/`, the flashed board) is still routed L3. **Not flashed, not hardware-tested** |
| One MAC & one IP (the host adopts the Pico's Wi-Fi MAC) | in code, in the L2 build: `bridge_init_mac()` copies `w0->hwaddr` into `tud_network_mac_address`, and `bridge_init()` brings cyw43 up **before** `tusb_init()` so the descriptor carries it. The default build still uses a board-ID MAC (`02:ce:50:d8:db:52`) |
| No NAT, no port-forwards | in code, in the L2 build: `BRIDGE_NAT_ENABLE` defaults to 0 there and `nat.c` contributes no symbols. The default build still runs NAT (`src/nat.c`, external ports from 50000) |
| No private subnet | in code, in the L2 build: `u1` is added with no address, TinyUSB's DHCP server is not compiled, and the console prints `mode: transparent L2 bridge (no NAT, no lease)`. The default build is still `172.28.0.0/24` with an 8-address pool |
| IPv4 & IPv6 | IPv4 only in both builds (`LWIP_IPV6 = 0`) |

| WPA2-PSK & WPA3-SAE | in code: a profile with a passphrase joins in transition mode, `CYW43_AUTH_WPA3_WPA2_AES_PSK` (`bridge_profile_auth()`). Never associated with a real AP yet |
| Management console on `/dev/ttyACM0`, debug console on `/dev/ttyACM1` | **measured on hardware** (2026-10-10): `CFG_TUD_CDC = 2`, six interfaces in the descriptor, REPL on `ttyACM0`, `ttyACM1` silent until `set debug on` and then carrying the `stats:` line |
| Runtime provisioning: profiles, `scan`/`join`/`save`, saved to flash | in code: 8 profiles, `cyw43_wifi_scan()`, settings in the last flash sector (magic + version + CRC32). Not yet exercised: the flashed board has 0 saved profiles |
| Watchdog, hard-fault handler, `RECOVERED from …` report | **measured on hardware** (2026-10-10): `test hang` → the board re-enumerated by itself and reported `RECOVERED from a hang` with `hangs=1`; `test fault` → `faultpc=0x10002572`, mapped by `addr2line` to the exact line that caused it; an lwIP assert reported with its own text |
| LED: solid / 1 Hz / 5 Hz / double flash / off | **measured on hardware** (2026-10-10): 100 ms per tick, 0.100 s per edge at 5 Hz, 0.500 s half-period at 1 Hz, double flash and the two steady patterns with no edges. Not yet validated as a *choice* for `solid` / `1hz` / `double` (no association, no profile, no live scan) and never looked at directly |
| ~4.5 Mbps throughput | **measured** (2026-10-10): 4 MB in 7.2 s = 4.4 Mbit/s, 20 MB in 32.9 s = 4.86 Mbit/s, with the flow pinned to the bridge interface. USB full-speed (12 Mbit/s) is the ceiling |
| Concurrent-flow limit | **measured** (2026-10-10): 20 concurrent long-lived TCP connections pass 80/80 requests; with the NAT table at `64/64`, 54 of 66 requests on live connections get no reply. A web page fits, a desktop session with an agent running does not |

"In code" means the source exists and the build is clean. It does **not** mean validated.
Measured on hardware means a line was read out of a board flashed with
`build/pico2_wifi_card.uf2` on 2026-10-10. Traceability and the open decisions are in
`.clinerules/00-requirements-baseline.md`.

Traceability, open decisions and technical risks: `.clinerules/00-requirements-baseline.md`.
The console specification to be met is `.clinerules/06-console-spec.md` (verbatim source text:
`openrouter-console-design.md.bak`). Closing these gaps means changing `src/`, not rewording
the requirements.

## Closing the gap: what the bridge needs, checked against the vendored SDK

- **lwIP already ships an IEEE 802.1D bridge.** `bridgeif.c` and `bridgeif_fdb.c` are part of
  `pico_lwip_netif` (`.pico-sdk/src/rp2_common/pico_lwip/CMakeLists.txt:104`) and their
  objects are already in `build/pico2_wifi_card.elf.map`. API: `bridgeif_init()` and
  `bridgeif_add_port(bridgeif, portif)`.
- **But it is compiled out today.** All of `bridgeif.c` sits inside
  `#if LWIP_NUM_NETIF_CLIENT_DATA` (`bridgeif.c:91` … `:563`), and `src/lwipopts.h` never
  defines `LWIP_NUM_NETIF_CLIENT_DATA`, so it defaults to `0`. Proof:
  `arm-none-eabi-nm build/pico2_wifi_card.elf | grep -c bridgeif` prints `0`. A bridge needs
  `LWIP_NUM_NETIF_CLIENT_DATA >= 1`.
- **The forwarding mode is already the right one.** `BRIDGEIF_PORT_NETIFS_OUTPUT_DIRECT`
  defaults to `NO_SYS` (`bridgeif_opts.h:57`), so with `NO_SYS = 1` a port's input calls the
  target port's `linkoutput` directly — no thread, which matches `bridge_poll()`.
- **`bridgeif` enforces "one IP per bridge".** `bridgeif_add_port()` clears
  `NETIF_FLAG_ETHARP` off each port (`bridgeif.c:558`), so ARP stops running per interface.
- **MAC adoption is possible but needs the init order flipped.** The station MAC comes from
  `cyw43_wifi_get_mac()`; TinyUSB publishes `tud_network_mac_address[6]` and builds the CDC-NCM
  `iMACAddress` string from it. Today `bridge_init()` fixes the MAC and calls `tusb_init()`
  before `cyw43_arch_init_with_country()` (`bridge.c:822-859`), so USB picks its MAC before the
  Wi-Fi MAC is known.
- **None of this can be tested before the data path works.** The board on the desk still runs
  an older image that carries no traffic. Flash `build/pico2_wifi_card.uf2` first.


## Build

The Pico SDK is vendored in `.pico-sdk`.

```sh
cmake -S . -B build -G Ninja -DPICO_SDK_PATH=$PWD/.pico-sdk
ninja -C build
```

Credentials are set on the console (`set ssid`, `set pass`, `set country`, then `save`), not
in the source. `src/config.h` keeps placeholders that are used only as the **seed for the
first profile when flash is empty**:

```c
#define BRIDGE_WIFI_SSID      "CHANGE_ME_SSID"
#define BRIDGE_WIFI_PASSWORD  "CHANGE_ME_PASSWORD"
#define BRIDGE_WIFI_AUTH      CYW43_AUTH_WPA2_AES_PSK   /* default only; see below */
#define BRIDGE_WIFI_COUNTRY   CYW43_COUNTRY_WORLDWIDE
```

A profile with a passphrase always joins in transition mode
(`bridge_profile_auth()` → `CYW43_AUTH_WPA3_WPA2_AES_PSK`), so `BRIDGE_WIFI_AUTH` no longer
selects the authentication used on the air.

Everything else on the USB side - address range, DHCP pool, lease time, NAT
table size and timeouts, watchdog period - is in the same file.

### Scratch build directories

`build/` is the reference image. Verification variants get their own directory so `build/`
stays the reference image; they are generated output, not source, and `.gitignore` ignores
them (`build-*/`):

```sh
# USB-only bisection: no Wi-Fi, no NAT - does the USB data path work on its own?
cmake -S . -B build-usbonly -G Ninja -DPICO_SDK_PATH=$PWD/.pico-sdk \
      -DBRIDGE_WIFI_ENABLE=0 -DBRIDGE_NAT_ENABLE=0
ninja -C build-usbonly

# one line per bridged frame on the debug console
cmake -S . -B build-trace -G Ninja -DPICO_SDK_PATH=$PWD/.pico-sdk -DTRACE_FRAMES=1
ninja -C build-trace

# self-test: a `test` verb on the console, to cause and watch a recovery
cmake -S . -B build-selftest -G Ninja -DPICO_SDK_PATH=$PWD/.pico-sdk -DBRIDGE_SELFTEST=1
ninja -C build-selftest

# transparent L2 bridge: no NAT, no private subnet, host adopts the station MAC
cmake -S . -B build-l2 -G Ninja -DPICO_SDK_PATH=$PWD/.pico-sdk -DBRIDGE_L2_BRIDGE=1
ninja -C build-l2
```

`build-usbonly` is the artifact for step 5 of the debugging order in
`.clinerules/03-build-and-test.md`: if DHCP works with that image, the USB path is fine and
the fault is on the cyw43 / lwIP timing side. Neither variant has been flashed.

`build-l2` is the transparent bridge the requirements ask for. It builds clean and is separate
from the reference image at the symbol level (`build-l2` has `bridge_l2_usb_input` /
`bridge_l2_wifi_input` and zero `bridge_hook_*`, `bridge_nat_*`, `nat_*` or `dhserv_init`
symbols; `build/` has the hooks and zero `bridge_l2_*`). **It has not been flashed**: flashing a
`uf2` destroys the saved Wi-Fi profile on this board, and without a profile there is no uplink
to bridge. See `.clinerules/03-build-and-test.md` ("`build-l2`").


`build-selftest` is the image the recovery checks were run on. `BRIDGE_SELFTEST` is 0 in
`src/config.h` and no shipped build turns it on: `build/` contains no `cmd_test`, no
`bridge_led_*` readout and no `test` string at all, and on the board the reference image answers
`test` with `[!] unknown command "test"`. See
`.clinerules/03-build-and-test.md` ("Validarea diagnosticelor fără rețea").



## Flash

Hold BOOTSEL, plug the Pico in, and copy `build/pico2_wifi_card.uf2` onto the boot
drive. The label depends on the board: `RPI-RP2` on RP2040 boards, **`RP2350`** on
the Pico 2 (verified here: `/media/cezar/RP2350`, `INFO_UF2.TXT` → `Board-ID: RP2350`).

`build/_deps/picotool/picotool` is built by the SDK **without USB support**, so
`picotool load -x build/pico2_wifi_card.uf2` needs a system `picotool` or a debug
probe; the BOOTSEL copy above is what works with the vendored build.


## Console

The USB port carries three functions: the CDC-NCM network interface, the management
console on `/dev/ttyACM0` and the debug stream on `/dev/ttyACM1`. UART0 is kept as a
mirror of the same state: GPIO0 = TX, GPIO1 = RX, 115200 8N1.

```sh
picocom /dev/ttyACM0            # management: set / list / scan / join / save / stats
picocom /dev/ttyACM1            # debug stream: mute until `set debug on`
picocom -b 115200 /dev/ttyUSB0  # UART0 mirror of the same state
```

The debug stream is silent until `set debug on` is typed on `/dev/ttyACM0`. Once on, it
reports association changes and one stats line every `BRIDGE_STATUS_INTERVAL_MS`:

```
stats: ->wifi=128 ->host=341 txdrop=0 rxdrop=0 refl=12 poolfail=0 ringpk=4 link=up hangs=0 faults=0 faultpc=0x00000000 freeram=424916 host=192.168.1.218
```

`reboot` restarts the board from the console and `reboot bootsel` brings the BOOTSEL drive
back, which is what makes reflashing possible without holding the button. Neither command is
in the specification's table; both are debug aids.


### Diagnostics measured without a network

The recovery paths need *causes*, and a board with nothing wired to GP0/GP1 has only USB.
`build-selftest` adds a `test` verb so each cause can be provoked and the recovery read back
from the host. Measured on the board (2026-10-10), on `build-selftest`:

| Command | What the board reported |
|---|---|
| `test hang` | re-enumerated by itself in ~4 s, no button, no unplug; `RECOVERED from a hang: the watchdog reset the firmware (->wifi=0 ->host=3 txdrop=128 rxdrop=0)`, then `hangs=1` in `stats:` |
| `test fault` | `RECOVERED from hard fault at 0x10002572 (hfsr=0x40000000 cfsr=0x00010000 …)`; `addr2line` maps `faultpc` onto the `__builtin_trap()` line in `src/console.c` |
| `test assert` | `RECOVERED from an lwIP assert: selftest: deliberate assert (->wifi=0 ->host=2 txdrop=86 rxdrop=0)` |
| `test led` | `led: 5hz level on tick 5 state asks 5hz` + `usb up wifi down profiles 0 live no` |

`test led <name> [seconds]` holds one of the five patterns so `scripts/led_probe.py` can time it
from the host: 100 ms per tick, 0.100 s per edge at 5 Hz, 0.500 s half-period at 1 Hz, 0.250 s
average for the double flash, no edges for solid and for off. The onboard LED is a CYW43 GPIO and
cannot be read back — `cyw43_ll_gpio_get()` sends the `ccgpioin` ioctl, the radio's input state,
not the `gpioout` value the firmware wrote — so these are the chooser's own decisions.

`scripts/led_choice_probe.py` answers the other half of the requirement: not what the waveform
looks like but which pattern `bridge_led_pattern_for()` actually picked. `double` (live scan) is
the one choice reachable without Wi-Fi credentials, and it is confirmed:

```
baseline (no scan running):
    [baseline] driven=5hz     level=off tick= 4  state asks=5hz     match
during a live scan:
    [live   ] driven=double  level=off tick= 6  state asks=double  match
after `back`:
    [back   ] driven=5hz     level=on  tick= 1  state asks=5hz     match
```

`solid` and `1hz` still need a real association or a saved profile to be checked as choices.

`test` exists only in that build. `build/` contains no `cmd_test` and no `test` string, and the
flashed reference image answers `test` with `[!] unknown command "test"`.
### The bridged path under load

`scripts/nat_pressure.sh` opens more concurrent flows than the NAT table held
(`BRIDGE_NAT_TABLE_SIZE`, 24 when the limit was found, 64 now): 40 concurrent `ping`s, then 30
TCP and 25 UDP flows, all
pinned to the CDC-NCM adapter. That is what makes `nat_snat()` fail inside
`bridge_nat_output()`, which is the interesting case.

```sh
WAVES=3 scripts/nat_pressure.sh                       # 3 x (40 ICMP + 30 TCP + 25 UDP)
python3 -u /tmp/conwatch.py /dev/ttyACM1 300          # keep ttyACM1 open while the load runs
python3 scripts/console_probe.py /dev/ttyACM0 stats   # read the counters during the load
```

Measured on the board (2026-10-10) with the fix in place, real uplink on `<SSID>`:

```
stats: ->wifi=753 ->host=39 txdrop=133 rxdrop=0 refl=0 poolfail=0 ringpk=0 link=up hangs=0 faults=0 faultpc=0x00000000 freeram=423876 host=172.28.0.10
```

`faults=0`, `hangs=0`, `poolfail=0`, and zero `RECOVERED` / assert lines on `ttyACM1` for the
whole session. `ping 1.1.1.1` through the bridge: 10/10, 0% loss. Host side: `TX 3902 packets`,
`errors 0 dropped 0`, and zero `NETDEV WATCHDOG` entries.

**The defect this found.** `bridge_nat_output()` used to free the pbuf when `nat_snat()` failed.
Its caller in bridged traffic is `ip4_forward()`, and `ip4_input()` frees that pbuf as soon as
`ip4_forward()` returns (`.pico-sdk/lib/lwip/src/core/ipv4/ip4.c:659` then `:667`), so the error
path was a second free of the same pbuf. It tripped `LWIP_ASSERT("pbuf_free: p->ref > 0", …)`
(`core/pbuf.c:757`), which this build routes to `diagnostics_lwip_assert()`. Rebuilt with the
free restored and flashed, the same load produced the assert verbatim on the console:

```
RECOVERED from an lwIP assert: pbuf_free: p->ref > 0 at 0x10007ff7
    (->wifi=29 ->host=21 txdrop=134 rxdrop=0)
```

`addr2line` puts `0x10007ff7` inside `pbuf_free.part.0`. On that image `faults` climbed 1 → 2
between two consecutive `stats:` reads with no other cause; on the fixed image it stayed `0`
through the whole soak. `nat_snat()` fails on a full NAT table, on a protocol with no port to
key on, and on a truncated transport header — all reachable from ordinary host traffic, so this
was not a corner case. The rule now written into `.clinerules/02-coding-standards.md`: a
`netif->output` installed by `ip4_forward()` must not free the pbuf on any path.

`stats:` has no field for `wifi_drop`, which is the counter a NAT-table exhaustion moves. The
occupancy of the NAT table is now on the console state dump as `nat: <active>/<size> flows`;
`wifi_drop` itself is still only on the UART0 status line.

### Why the host loses the internet when its wired NIC goes down

Measured, not assumed. With the wired NIC down, the only default route left is the bridge's
(`default via 172.28.0.1 dev enx02ce50d8db52`), the Pico answers on its own addresses, and yet
new connections fail. The console shows why:

```
nat:        24/24 flows
flux nou 10.7.7.52 -> 0 received      <- a NEW flow gets nothing
internet   1.1.1.1 -> 0 received      a flow already in the table also lost its slot
punte      172.28.0.1 -> 3 received   the Pico itself: no NAT entry needed, always answers
```

The chain is: the wired NIC carries the host's default route while it is up, so the bridge only
ever sees a trickle and works. Pull the wired NIC and the whole desktop — browser, tailscale,
docker, telemetry — moves onto the bridge at once. That is more concurrent flows than the NAT
table held, `nat_create()` had nowhere to put them, and a refused flow looks exactly like a dead
adapter: the host sees silence. Existing flows kept working the whole time, which is what
distinguishes this from a broken bridge.

Two changes followed, both validated on the board:

* `BRIDGE_NAT_TABLE_SIZE` 24 → 64 (1 kB of static table, `nat_table` = `0x400` in the ELF map).
* `nat_create()` no longer refuses when the table is full: it takes the slot that has been idle
  the longest, the rule a real NAT applies, and counts the eviction in `nat_expired`.

After both, the same test with the wired NIC down: **40 new flows out of 40 succeeded**, table at
`61/64`, `poolfail=0`, `faults=0`, `hangs=0`, and the 3-wave soak afterwards stayed clean
(`->wifi=14348`, host `RX 3218 pk / errors 0 dropped 0`, zero `NETDEV WATCHDOG`).

**How to run that test without cutting your own connection.** Taking the wired NIC down makes
the bridge the only default route, so everything on the host — including an agent working from
this host, which is what produced these measurements — starts riding the bridge. If the NAT
table saturates at that moment, the connection drops mid-measurement and the run cannot be
reported at all. `scripts/pinned_flow.sh` gets the same flows through the bridge without
touching the wired NIC: `ping -I <interface>` uses `SO_BINDTODEVICE`, so the test traffic leaves
through the bridge while the host keeps its wired default route.

```sh
nmcli connection modify "Wired connection 4" ipv4.route-metric 600
nmcli connection up "Wired connection 4"
ip route get 1.1.1.1                        # must show enp7s0, not enx...
scripts/pinned_flow.sh enx02ce50d8db52 40   # 40 new flows, wired NIC untouched
```

Measured 2026-10-10 with the wired NIC untouched (metrics 105 wired / 600 bridge): **40 new
flows out of 40 succeeded**, `nat: 43/64`, `poolfail=0`, `faults=0`, `hangs=0`, host
`RX 9053 pk / errors 0 dropped 0`.

This is a mitigation, not the requirement. The requirements reference asks for a transparent L2
bridge with one MAC and one address and **no NAT**; a per-flow table with a fixed size is a
consequence of the L3 + NAT design that the requirement says must go away.




> Read out of a board on 2026-10-10 (`build/pico2_wifi_card.uf2` flashed): the management
> console answers on `/dev/ttyACM0` (banner, prompt, full state dump, `set debug on`,
> `reboot bootsel`), `/dev/ttyACM1` stays silent until `set debug on` and then carries the
> `stats:` line, and `host IPv4` is snooped from bridged traffic. Provisioning works: `scan`
> listed two real networks with RSSI and BSSID, `live` streamed them, `back` left the submenu,
> and `set ssid` / `set pass` / `save` / `list` / `del` / `restore` all behaved — `restore`
> reads the record straight out of flash. `stats:` reports `link=nonet` for an SSID that does
> not exist.
>
> One thing measured and **wrong**: a saved profile survives a reset but **does not survive
> reflashing the `.uf2`** — after the copy, `list` and `restore` both report 0 profiles, even
> though the image occupies the first ~342 kB of a 4 MB chip and the record lives in the last
> 4 kB sector. Tracked in `.clinerules/05-diagnostics-and-console.md`.

## Host side

Plug the Pico into a USB port and the host should enumerate a CDC-NCM adapter and
take a lease automatically.

> Measured on Linux (2026-10-10, image flashed): enumeration works (`2e8a:4022`,
> `cdc_ncm` bound, `enx02ce50d8db52`, MAC `02:ce:50:d8:db:52`), the host takes a lease
> (`172.28.0.10/24`) and the data path carries frames (`RX`/`TX` with real packets,
> `errors 0 dropped 0`, no `NETDEV WATCHDOG`). `ping 172.28.0.1` - 0% loss, ~1.8 ms.
> The earlier signature (`RX: 0 packets`, `NETDEV WATCHDOG`, `no lease`) had a cause:
> `bridge_checksum_fixup()` wrote a zero IP header checksum into every frame lwIP built
> itself, so the host accepted the bytes and discarded the frames. See
> `03-build-and-test.md` in `.clinerules/` for the trace that proved it and for
> `scripts/frame_decode.py`, which re-checks it frame by frame.


* **Linux** - NetworkManager or systemd-networkd usually just picks it up. By
  hand:
  ```sh
  ip link set usb0 up
  dhclient usb0            # or: ip addr add 172.28.0.10/24 dev usb0
  ping 172.28.0.1          # the Pico
  ping 1.1.1.1             # through NAT
  ```
* **Windows 10/11** - the device advertises USB 2.1 (bcdUSB `0x0201`) with a BOS
  descriptor and a Microsoft OS 2.0 descriptor set whose compatible ID is
  `WINNCM`, so the built-in NCM driver binds without any manual install.
* **macOS** - CDC-NCM is supported natively.

## How the pieces fit

* `src/bridge.c` - the two netifs, the USB RX ring, TinyUSB's network callbacks,
  the Wi-Fi connect/retry loop, the DHCP server, the five LED patterns and the two
  lwIP hooks.
* `src/console.c` - the REPL on `/dev/ttyACM0`, the debug stream on `/dev/ttyACM1`,
  the eight profiles and the settings record in the last flash sector.
* `src/diagnostics.c` - watchdog, hard-fault handler, the crash record in
  `.uninitialized_data`, and the counters the `stats:` line reports.
* `src/nat.c` - the NAT table, outbound source NAT, inbound NAT undo, expiry.
* `src/bridge_hooks.h` + `LWIP_HOOK_*` in `src/lwipopts.h`:
  * `LWIP_HOOK_IP4_ROUTE_SRC` answers "which interface does a packet with this
    source leave on?" - USB-side sources go out Wi-Fi, Wi-Fi-side sources aimed
    at `172.28.0.0/24` go out USB.
  * `LWIP_HOOK_IP4_INPUT` runs before lwIP decides whether a packet is for this
    device, which is where inbound NAT is undone so lwIP forwards the reply on.
* `netif->output` on the Wi-Fi side is replaced with `bridge_nat_output()`.
* lwIP zeroes the IP and transport checksums when it forwards a packet, so both
  output paths rebuild them with `bridge_checksum_fixup()`.

See `src/dhcp_notes.md` for why the DHCP server's replies need the route hook.

## Known limitations

* NAT keys on ports, so only **TCP, UDP and ICMP echo** are translated. Other
  protocols (GRE, ESP, ICMP errors, ...) leave with the Pico's address and never
  come back.
* Fragmented datagrams are not translated: a continuation fragment carries no
  port to key on. The address is rewritten, the mapping is not.
* No inbound port forwarding - a host behind the USB side can only be reached by
  traffic it asked for first.
* IPv4 only. IPv6 is disabled in `src/lwipopts.h`.
* One USB host at a time, and the DHCP pool is 8 addresses.
* `CFG_TUD_NCM_*` and the lwIP pools in `src/lwipopts.h` are sized for light
  browsing, not for saturating a fast link.
* Several items above are deviations from the requirements reference (NAT and the private
  subnet, IPv4-only). They are tracked as unmet requirements in
  `.clinerules/00-requirements-baseline.md`. The console, the watchdog, the hard-fault handler,
  the lwIP-assert report and the five LED patterns are not in that list: they exist in `src/`
  and were measured on hardware (see `.clinerules/05-diagnostics-and-console.md`). The Wi-Fi
  side is validated too: the station associates with a real WPA2 access point, takes a lease
  from the router, and bridged traffic crosses the bridge under load (see
  "The bridged path under load"). What remains measured-and-wrong is profile persistence across
  a `.uf2` reflash, and what remains unvalidated is WPA3-SAE on a real access point.
* `stats:` has no field for `wifi_drop`, the counter a NAT-table exhaustion moves. NAT table
  occupancy is now visible as `nat: <active>/<size> flows` in the console state dump; a full
  table is no longer only a silence.
