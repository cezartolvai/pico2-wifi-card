/* ---------------------------------------------------------------------------
 * bridge_hooks.h - included by lwIP itself (see LWIP_HOOK_FILENAME in
 * lwipopts.h) in every file that offers a hook point.
 *
 * LWIP_HOOK_IP4_INPUT  - runs in ip4_input() before lwIP decides whether the
 *                        packet is for us.  Used to undo NAT on inbound
 *                        packets so that lwIP forwards them to the USB host.
 * LWIP_HOOK_IP4_ROUTE_SRC - decides which interface a forwarded (or locally
 *                        generated) packet leaves on.
 * ------------------------------------------------------------------------ */
#ifndef PICO2_WIFI_CARD_BRIDGE_HOOKS_H
#define PICO2_WIFI_CARD_BRIDGE_HOOKS_H

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"

/* Return non-zero only if the hook took ownership of \a p (and freed it). */
int bridge_hook_ip4_input(struct pbuf *p, struct netif *inp);

/* Return NULL to let lwIP fall back to its own (default-netif) routing. */
struct netif *bridge_hook_route_src(const ip4_addr_t *src, const ip4_addr_t *dest);

/* The macros themselves live in lwipopts.h so that lwIP's own headers see them
 * early enough (see the note there).  Redefining them here would be harmless,
 * but guarding keeps the intent obvious. */
#ifndef LWIP_HOOK_IP4_INPUT
#define LWIP_HOOK_IP4_INPUT(p, inp)         bridge_hook_ip4_input((p), (inp))
#endif
#ifndef LWIP_HOOK_IP4_ROUTE_SRC
#define LWIP_HOOK_IP4_ROUTE_SRC(src, dest)  bridge_hook_route_src((src), (dest))
#endif

#endif /* PICO2_WIFI_CARD_BRIDGE_HOOKS_H */
