/* ---------------------------------------------------------------------------
 * config.h - the knobs you are likely to touch.
 *
 * The device has two sides:
 *   Wi-Fi side : a normal station on your home network (address from DHCP).
 *   USB  side  : a private network handed to the computer that plugs it in.
 * ------------------------------------------------------------------------ */
#ifndef PICO2_WIFI_CARD_CONFIG_H
#define PICO2_WIFI_CARD_CONFIG_H

/* ------------------------------ Wi-Fi uplink ---------------------------- */
/* IP4_ADDR() counts its arguments before expanding them, so a macro that
 * expands to "172, 28, 0" cannot be passed to it directly.  Wrap it:
 *
 *      BRIDGE_IP4_ADDR(&addr, BRIDGE_HOST_IP)
 *
 * expands to IP4_ADDR((&addr), 172, 28, 0, 1), which is what you wanted.
 */
#define BRIDGE_IP4_ADDR(addr, ...)   IP4_ADDR((addr), __VA_ARGS__)

/* 0 builds the bridge without the cyw43 side at all: no cyw43 init, no connect
 * attempts, no cyw43_arch_poll().  It exists to bisect the USB data path - if
 * the host still gets no lease with the Wi-Fi side gone, the fault is on the
 * USB/lwIP side and not in cyw43.  The onboard LED is a CYW43 GPIO, so it is
 * dark in this mode; do not ship a build with this set to 0.                  */
#ifndef BRIDGE_WIFI_ENABLE
#define BRIDGE_WIFI_ENABLE      1
#endif

#ifndef BRIDGE_WIFI_SSID
#define BRIDGE_WIFI_SSID        "CHANGE_ME_SSID"
#endif
#ifndef BRIDGE_WIFI_PASSWORD
#define BRIDGE_WIFI_PASSWORD    "CHANGE_ME_PASSWORD"
#endif
/* CYW43_AUTH_WPA2_AES_PSK | CYW43_AUTH_WPA2_MIXED_PSK | CYW43_AUTH_OPEN */
#ifndef BRIDGE_WIFI_AUTH
#define BRIDGE_WIFI_AUTH        CYW43_AUTH_WPA2_AES_PSK
#endif
/* CYW43_COUNTRY_WORLDWIDE, CYW43_COUNTRY_GB, CYW43_COUNTRY_US, ... */
#ifndef BRIDGE_WIFI_COUNTRY
#define BRIDGE_WIFI_COUNTRY     CYW43_COUNTRY_WORLDWIDE
#endif
/* Seconds to wait before retrying a failed Wi-Fi connection attempt. */
#ifndef BRIDGE_WIFI_RETRY_SECONDS
#define BRIDGE_WIFI_RETRY_SECONDS 10
#endif

/* ------------------------- transparent L2 bridging ------------------------ */
/* What the two modes actually are:

 * BRIDGE_L2_BRIDGE = 0  (the default, and what build/ contains today)
 *      lwIP terminates IP on both interfaces and routes between them:
 *      IP_FORWARD + LWIP_HOOK_IP4_ROUTE_SRC + a private 172.28.0.0/24 handed to
 *      the host by a DHCP server running on the Pico + source NAT on the way
 *      out.  The host is one hop behind the Pico, and the NAT table is a hard
 *      ceiling on concurrent flows (measured: 64 flows, and past it a live
 *      connection simply stops getting replies).
 *
 * BRIDGE_L2_BRIDGE 1  (what the requirements ask for)
 *      The Pico forwards Ethernet frames.  A frame from the host leaves on the
 *      Wi-Fi interface unchanged; a frame from the access point goes back out
 *      to the host.  No IP decision, so no routing table, no address
 *      translation and no per-flow mapping to run out of.  The host adopts the
 *      station's MAC, takes the router's own DHCP lease, and the Pico holds no
 *      address at all.
 *
 * It is 0 here so that build/ stays the reference image until the L2 path has
 * been validated on hardware; -DBRIDGE_L2_BRIDGE=1 builds the bridge itself.
 * See .clinerules/00-requirements-baseline.md (R2, R3, R4, R5). */
#ifndef BRIDGE_L2_BRIDGE
#define BRIDGE_L2_BRIDGE        0
#endif

/* ------------------- USB side: the network the PC joins ----------------- */
/* The Pico takes .1 of this network; the host is leased an address from the
   pool below.  172.28.0.0/24 is chosen because it almost never collides with
   a home router (192.168.x.x) or with Docker's default (172.17.x.x).        */
#ifndef BRIDGE_HOST_NET_PREFIX
#define BRIDGE_HOST_NET_PREFIX      172, 28, 0
#endif
#ifndef BRIDGE_HOST_IP_LAST
#define BRIDGE_HOST_IP_LAST         1
#endif
#define BRIDGE_HOST_IP              BRIDGE_HOST_NET_PREFIX, BRIDGE_HOST_IP_LAST
#ifndef BRIDGE_HOST_NETMASK
#define BRIDGE_HOST_NETMASK         255, 255, 255, 0
#endif
/* Last octet of the first / last leased address, and how many there are. */
#ifndef BRIDGE_HOST_POOL_FIRST
#define BRIDGE_HOST_POOL_FIRST      10
#endif
#ifndef BRIDGE_HOST_POOL_SIZE
#define BRIDGE_HOST_POOL_SIZE       8
#endif
#ifndef BRIDGE_HOST_LEASE_SECONDS
#define BRIDGE_HOST_LEASE_SECONDS   3600
#endif
#ifndef BRIDGE_HOST_DOMAIN
#define BRIDGE_HOST_DOMAIN          "pico2.local"
#endif
#ifndef BRIDGE_HOST_MTU
#define BRIDGE_HOST_MTU             1500
#endif

/* ---------------------------------- NAT --------------------------------- */
/* Off by default in the transparent build: a bridge that forwards Ethernet
 * frames has no address to translate into, so a NAT table would be dead code
 * and -DBRIDGE_L2_BRIDGE=1 alone would leave it compiled in. */
#ifndef BRIDGE_NAT_ENABLE
#if BRIDGE_L2_BRIDGE
#define BRIDGE_NAT_ENABLE       0
#else
#define BRIDGE_NAT_ENABLE       1
#endif
#endif
#ifndef BRIDGE_NAT_TABLE_SIZE
#define BRIDGE_NAT_TABLE_SIZE   64
#endif
/* lwIP gives its own sockets ephemeral ports starting at 49152, so the NAT
 * range starts above that to keep the Pico's own traffic distinguishable from
 * the traffic it is translating. */
#ifndef BRIDGE_NAT_PORT_BASE
#define BRIDGE_NAT_PORT_BASE    50000
#endif
#ifndef BRIDGE_NAT_TCP_IDLE_MS
#define BRIDGE_NAT_TCP_IDLE_MS  60000
#endif
#ifndef BRIDGE_NAT_UDP_IDLE_MS
#define BRIDGE_NAT_UDP_IDLE_MS  30000
#endif
#ifndef BRIDGE_NAT_ICMP_IDLE_MS
#define BRIDGE_NAT_ICMP_IDLE_MS 15000
#endif

/* ---------------------------------- DNS --------------------------------- */
/* Advertised to the USB host as its resolver until the Wi-Fi side has been
 * given one by its own DHCP server, after which that one is used instead.
 * Queries are not intercepted - they are simply NATed through like any other
 * traffic - so a host that hard-codes 8.8.8.8 works too.                    */
#ifndef BRIDGE_FALLBACK_DNS
#define BRIDGE_FALLBACK_DNS     1, 1, 1, 1
#endif

/* ---------------------------- USB receive queue ------------------------- */
#ifndef BRIDGE_USB_RX_SLOTS
#define BRIDGE_USB_RX_SLOTS     6
#endif
#ifndef BRIDGE_MAX_FRAME
#define BRIDGE_MAX_FRAME        1514
#endif

/* --------------------------- settings in flash --------------------------- */
/* The record keeps the tail of the chip - separate from the program image at
 * the start, which is what the specification asks - but it no longer sits in
 * the LAST 4 kB of the chip.
 *
 * Flashing an RP2350 carries the RP2350-E10 absolute block (family ABSOLUTE,
 * extension UF2_EXTENSION_RP2_IGNORE_BLOCK) targeting 0x10ffff00, the top of the
 * flash address window.  On a 4 MB part that address lands in the final 4 kB
 * sector, and the erase which accompanies the load wipes that sector whatever
 * the size of the image - measured here with an 81 kB image, and stated by the
 * SDK maintainers in pico-sdk#1943: "this drag & drop will unnecessarily erase
 * the last 4KiB of connected flash", while "picotool ... will ignore any
 * E10 abs-blocks".
 *
 * So the record sits BRIDGE_SETTINGS_RESERVE_SECTORS below the top: clear of
 * that alias, clear of the sectors picotool reserves for the BTStack flash bank,
 * and still far above the program image (0x10000000..0x10053524).  Widen the
 * guard with -DBRIDGE_SETTINGS_RESERVE_SECTORS=<n> if a loader is ever measured
 * erasing more than this; `test peek` in a self-test build is how that span is
 * measured. */
#ifndef BRIDGE_SETTINGS_RESERVE_SECTORS
#define BRIDGE_SETTINGS_RESERVE_SECTORS 16
#endif


/* ------------------------------ diagnostics ----------------------------- */
/* 0 = quiet, 1 = status line every BRIDGE_STATUS_INTERVAL_MS. */
#ifndef BRIDGE_VERBOSE
#define BRIDGE_VERBOSE          1
#endif
#ifndef BRIDGE_STATUS_INTERVAL_MS
#define BRIDGE_STATUS_INTERVAL_MS 15000
#endif
/* Onboard LED tick.  Every pattern in table 3 is a multiple of it: 100 ms
 * gives 5 Hz by alternating, 1 Hz by counting five on and five off. */
#ifndef BRIDGE_LED_TICK_MS
#define BRIDGE_LED_TICK_MS      100
#endif
/* Watchdog timeout.  The loop must reload it more often than this; the longest
 * thing the firmware does in one pass is a flash erase+program on `save`, which
 * is tens of milliseconds, so 4 s is a wide margin over the work and short
 * enough that a locked board re-enumerates in about that time.
 * The hardware caps this at 8388 ms. */
#ifndef BRIDGE_WATCHDOG_MS
#define BRIDGE_WATCHDOG_MS      4000
#endif
/* How long a recovery report is held waiting for a CDC console to exist.  The
 * watchdog can fire seconds after boot, before the host has configured the
 * CDC-ACM functions; the line is kept this long, then UART0 keeps it alone. */
#ifndef BRIDGE_RECOVERY_HOLD_MS
#define BRIDGE_RECOVERY_HOLD_MS 10000
#endif
/* 1 = one line per bridged frame on the debug stream.  It floods the console
 * under load, so it stays off unless the build asks for it. */
#ifndef TRACE_FRAMES
#define TRACE_FRAMES            0
#endif
/* Blink the (CYW43) onboard LED: solid when the Wi-Fi link is up. */
#ifndef BRIDGE_LED
#define BRIDGE_LED              1
#endif

/* 1 = add the `test` verb to the management console: `test hang`, `test fault`,
 * `test assert`, `test led`.
 *
 * It exists because the recovery paths of src/diagnostics.c cannot be validated
 * any other way on a board whose GP0/GP1 are not wired to anything: a watchdog
 * timeout, a CPU fault and an lwIP assert each have to be *caused* on purpose
 * before the board can be watched recovering from them, and the only channel to
 * this board is USB.  `test led` reports which of the five LED patterns the
 * chooser picked, so the pattern can be confirmed from the host.
 *
 * 0 in every image meant to be shipped: a test build is its own build directory
 * (build-selftest), never build/.                                                            */
#ifndef BRIDGE_SELFTEST
#define BRIDGE_SELFTEST         0
#endif


#endif /* PICO2_WIFI_CARD_CONFIG_H */
