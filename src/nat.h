/* ---------------------------------------------------------------------------
 * nat.h - the IPv4 network address translator on the Wi-Fi side.
 * ------------------------------------------------------------------------ */
#ifndef PICO2_WIFI_CARD_NAT_H
#define PICO2_WIFI_CARD_NAT_H

#include <stdbool.h>
#include <stdint.h>

#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip4.h"

/* Replacement for the Wi-Fi netif's netif->output: source-NAT packets that
 * come from the USB-side network, fix up checksums, then hand the packet to
 * etharp. */
err_t bridge_nat_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr);

/* Undo NAT on an inbound packet: rewrite the destination so that lwIP routes
 * the packet to the USB host instead of dropping it.  The packet is modified
 * in place; lwIP keeps processing it.  Returns true if a mapping matched. */
bool bridge_nat_unnat(struct netif *inp, struct pbuf *p, struct ip_hdr *iphdr);

/* Reap idle mappings.  Call from the main loop. */
void bridge_nat_expire(void);

unsigned bridge_nat_active(void);

#endif /* PICO2_WIFI_CARD_NAT_H */
