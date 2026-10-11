/* ---------------------------------------------------------------------------
 * bridge.h - the USB <-> Wi-Fi bridge.
 * ------------------------------------------------------------------------ */
#ifndef PICO2_WIFI_CARD_BRIDGE_H
#define PICO2_WIFI_CARD_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lwip/netif.h"
#include "lwip/pbuf.h"

#include "config.h"            /* BRIDGE_SELFTEST gates the LED readout below */

/* lwIP interface facing the USB host (TinyUSB CDC-NCM). */
extern struct netif bridge_usb_netif;

/* lwIP interface facing your Wi-Fi network (owned by pico_cyw43_arch). */
struct netif *bridge_wifi_netif(void);

typedef struct {
    uint32_t usb_rx;        /* frames taken from the USB host            */
    uint32_t usb_rx_drop;   /* frames dropped before reaching lwIP       */
    uint32_t usb_tx;        /* frames handed to the USB host             */
    uint32_t usb_tx_drop;   /* frames dropped because USB was busy/gone  */
    uint32_t wifi_tx;       /* frames put on the Wi-Fi network           */
    uint32_t wifi_drop;     /* frames dropped on the Wi-Fi side          */
    uint32_t forwarded;     /* routing decisions that chose an interface */
    uint32_t no_route;      /* packets with nowhere to go                */
    uint32_t nat_out;       /* outbound addresses/ports rewritten        */
    uint32_t nat_in;        /* inbound addresses/ports rewritten         */
    uint32_t nat_new;       /* mappings created                          */
    uint32_t nat_expired;   /* mappings reaped                           */

    /* Fields the stats: line of the debug console reports (table 2 of
     * .clinerules/06-console-spec.md).  forwarded counts decisions, not
     * frames, so the frames that really crossed the bridge are counted here. */
    uint32_t to_wifi;       /* ->wifi: host->Wi-Fi frames transmitted    */
    uint32_t refl;          /* host's own frames bounced back by the AP  */
    uint32_t poolfail;      /* lwIP's pbuf pool was momentarily empty    */
    uint16_t ringpk;        /* peak USB queue depth since the last stats */
} bridge_stats_t;


extern bridge_stats_t bridge_stats;

/* Bring up cyw43 + lwIP + the USB netif + the DHCP server. */
void bridge_init(void);

/* Service USB, Wi-Fi and the lwIP timers.  Call in a tight loop. */
void bridge_poll(void);

/* True once both interfaces exist and the Wi-Fi side has an address. */
bool bridge_wifi_ready(void);
bool bridge_usb_ready(void);

/* True when cyw43 came up: without it there is no radio to talk about. */
bool bridge_wifi_present(void);

/* Drop the current association and start again with the active profile, so a
 * console change takes effect without a reboot. */
void bridge_wifi_restart(void);

/* Words for the console's state dump.  bridge_status_word() reports the STA
 * link ("up", "join", "badauth", "nonet", "fail", "unconfigured");
 * bridge_host_ipv4_word() reports the address seen on the host's own frames. */
const char *bridge_status_word(void);
const char *bridge_link_word(void);
const char *bridge_host_ipv4_word(void);
const char *bridge_wifi_ipv4_word(void);
const char *bridge_wifi_gateway_word(void);

void bridge_set_led(bool on);
void bridge_print_status(void);

#if BRIDGE_SELFTEST
/* What bridge_led_service() did on its last tick: the pattern it drove, the
 * pattern its state asked for (not the same thing while `test led <name>` holds
 * one), the level written, and the place in the ten-tick cycle.  A CYW43 output
 * pin cannot be read back (cyw43_ll_gpio_get() reads the radio's "ccgpioin"
 * input state, not the "gpioout" value that was written), so this is how a
 * pattern gets confirmed without eyes on the board.  Declared only for a
 * self-test build: a build with BRIDGE_SELFTEST=0 contains neither these nor
 * the `test` verb. */
const char *bridge_led_pattern(void);
const char *bridge_led_state(void);
bool bridge_led_level(void);
uint32_t bridge_led_tick(void);
bool bridge_led_force(const char *pattern, uint32_t seconds);
#endif /* BRIDGE_SELFTEST guard is applied at the use site in console.c */

/* The periodic line of the debug console, formatted exactly as table 2 of
 * .clinerules/06-console-spec.md.  Returns the number of characters written. */
size_t bridge_format_stats(char *buf, size_t len);

/* Recompute the IPv4 header checksum and the TCP/UDP/ICMP checksum of the
 * packet whose IP header is at p->payload.  lwIP zeroes these when it
 * forwards a packet, so every output path of this bridge has to redo them. */
/* Recompute the IPv4 header checksum and the TCP/UDP/ICMP checksum of the
 * packet whose IP header is at p->payload.  lwIP zeroes these when it
 * forwards a packet, so every output path of this bridge has to redo them. */
void bridge_checksum_fixup(struct pbuf *p);

/* One line about one frame, on the debug stream.  Compiles down to nothing
 * unless the build is configured with -DTRACE_FRAMES=1, which is what the
 * specification asks for; it is not a command because it floods the console. */
void bridge_trace_frame(const char *dir, const ip4_addr_t *src, const ip4_addr_t *dest,
                       uint8_t proto, uint16_t len);


#endif /* PICO2_WIFI_CARD_BRIDGE_H */
