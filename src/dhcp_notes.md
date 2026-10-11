# The DHCP server on the USB side

`src/bridge.c` uses the DHCP server that ships with TinyUSB:

```
${PICO_TINYUSB_PATH}/lib/networking/dhserver.c
```

* Upstream: https://github.com/hathach/tinyusb — `lib/networking/dhserver.{c,h}`
* Author: Sergey Fetisov, MIT licensed (see TinyUSB's `LICENSE`).
* It is a miniature raw-API lwIP DHCP server, not lwIP's own `LWIP_DHCP`
  (which is client-only in this project and is used on the Wi-Fi interface).

## How it is wired up

```c
static dhcp_entry_t dhcp_entries[BRIDGE_HOST_POOL_SIZE];
static dhcp_config_t  dhcp_config;

dhcp_config.port      = 67;                 /* DHCP_SERVER_PORT   */
dhcp_config.num_entry = BRIDGE_HOST_POOL_SIZE;
dhcp_config.entries   = dhcp_entries;       /* one address per slot */
dhcp_config.domain    = BRIDGE_HOST_DOMAIN;
IP4_ADDR(&dhcp_config.router, BRIDGE_HOST_IP);   /* 172.28.0.1 */
IP4_ADDR(&dhcp_config.dns,    BRIDGE_FALLBACK_DNS);
dhserv_init(&dhcp_config);
```

Each entry carries a MAC, an address and a lease time.  `entry_by_mac()` reuses
the address a host already has; `vacant_address()` hands out the next free one.
`bridge_refresh_dns()` copies whatever the Wi-Fi side's DHCP client learned into
`dhcp_config.dns`, so the host gets the real resolver once it is known.

## Two things to know about it

1. **It replies on the interface the request arrived on.**  `udp_recv_proc()`
   recovers that with `netif_get_by_index(p->if_idx)`, so the OFFER/ACK carries
   the USB netif's address and netmask.  Good.

2. **It sends to `255.255.255.255` from a pcb bound to `0.0.0.0`.**  A limited
   broadcast matches no netif, so lwIP's `ip4_route()` would fall back to
   `netif_default` — which is the Wi-Fi interface here, because cyw43 claims it.
   `bridge_hook_route_src()` catches "no source, broadcast destination" and
   returns the USB interface instead.  Without that, DHCP offers would be sent
   out over Wi-Fi and the host would never get an address.

## What it does not do

* It never sends NAKs, and it does not honour `DHCP_REQUEST` for an address that
  is already leased to a different MAC — the request is simply ignored, and the
  client retries.
* It does not respond to `DHCP_INFORM`.
* It has no notion of the pool being exhausted beyond `vacant_address()`
  returning NULL, which is why `BRIDGE_HOST_POOL_SIZE` is the hard limit on how
  many hosts the USB side can have at once.
