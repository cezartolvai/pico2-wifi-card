/* ---------------------------------------------------------------------------
 * nat.c - a small port-address translation table on the Wi-Fi side.
 *
 * Outbound:  a packet whose source belongs to the USB-side network gets the
 *            Wi-Fi address and a port taken from this table.
 * Inbound:   a packet arriving on the Wi-Fi side whose destination port is in
 *            the table gets its destination rewritten back to the USB host, and
 *            lwIP forwards it from there (see bridge_hook_ip4_input).
 *
 * Only TCP, UDP and ICMP echo are translated.  Anything else leaves with the
 * wrong source address and never comes back - a limitation of a table keyed by
 * port, not something worth hiding.
 * ------------------------------------------------------------------------ */
#include <string.h>

#include "lwip/ip4.h"
#include "lwip/ip4_addr.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"
#include "lwip/prot/udp.h"
#include "lwip/sys.h"
#include "lwip/timeouts.h"
#include "netif/etharp.h"

#include "bridge.h"
#include "config.h"
#include "nat.h"

#if !BRIDGE_NAT_ENABLE
err_t bridge_nat_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr)
{
    return etharp_output(netif, p, ipaddr);
}
bool bridge_nat_unnat(struct netif *inp, struct pbuf *p, struct ip_hdr *iphdr)
{
    (void)inp; (void)p; (void)iphdr;
    return false;
}
void bridge_nat_expire(void) {}
unsigned bridge_nat_active(void) { return 0; }
#else /* BRIDGE_NAT_ENABLE */

typedef struct {
    bool       in_use;
    uint8_t    proto;      /* IP_PROTO_TCP / _UDP / _ICMP        */
    ip4_addr_t int_ip;     /* address on the USB side            */
    uint16_t   int_port;   /* port on the USB side, host order   */
    uint16_t   ext_port;   /* port we present on the Wi-Fi side  */
    uint32_t   last_ms;    /* sys_now() of the last packet seen  */
} nat_entry_t;

static nat_entry_t nat_table[BRIDGE_NAT_TABLE_SIZE];

/* ------------------------------- table ----------------------------------- */
static nat_entry_t *nat_find_out(uint8_t proto, const ip4_addr_t *int_ip, uint16_t int_port)
{
    for (unsigned i = 0; i < BRIDGE_NAT_TABLE_SIZE; i++) {
        nat_entry_t *e = &nat_table[i];
        if (e->in_use && (e->proto == proto) &&
            ip4_addr_eq(&e->int_ip, int_ip) && (e->int_port == int_port)) {
            return e;
        }
    }
    return NULL;
}

static nat_entry_t *nat_find_in(uint8_t proto, uint16_t ext_port)
{
    for (unsigned i = 0; i < BRIDGE_NAT_TABLE_SIZE; i++) {
        nat_entry_t *e = &nat_table[i];
        if (e->in_use && (e->proto == proto) && (e->ext_port == ext_port)) {
            return e;
        }
    }
    return NULL;
}

/* The external port is the slot number, which makes collisions inside the
 * table impossible by construction. */
static nat_entry_t *nat_create(uint8_t proto, const ip4_addr_t *int_ip, uint16_t int_port)
{
    uint32_t now = sys_now();
    unsigned slot = 0;
    bool free_slot = false;

    for (unsigned i = 0; i < BRIDGE_NAT_TABLE_SIZE; i++) {
        if (!nat_table[i].in_use) {
            slot = i;
            free_slot = true;
            break;
        }
    }

    if (!free_slot) {
        /* Not a corner case.  A desktop host opens more concurrent flows than
         * this table holds, and a refused flow is indistinguishable from a dead
         * bridge: the host sees silence and concludes the adapter is broken.
         * Reuse the mapping that has been idle the longest - the rule a real
         * NAT applies - and count the eviction, because the flow that lost its
         * slot is a real loss and not a free win. */
        uint32_t oldest = 0;

        for (unsigned i = 0; i < BRIDGE_NAT_TABLE_SIZE; i++) {
            uint32_t age = (uint32_t)(now - nat_table[i].last_ms);
            if ((i == 0u) || (age > oldest)) {
                oldest = age;
                slot = i;
            }
        }
        bridge_stats.nat_expired++;
    }

    nat_entry_t *e = &nat_table[slot];
    e->in_use   = true;
    e->proto    = proto;
    ip4_addr_copy(e->int_ip, *int_ip);
    e->int_port = int_port;
    e->ext_port = (uint16_t)(BRIDGE_NAT_PORT_BASE + slot);
    e->last_ms  = now;
    bridge_stats.nat_new++;
    return e;
}


static uint32_t nat_idle_ms(uint8_t proto)
{
    switch (proto) {
        case IP_PROTO_TCP:  return BRIDGE_NAT_TCP_IDLE_MS;
        case IP_PROTO_UDP:  return BRIDGE_NAT_UDP_IDLE_MS;
        default:            return BRIDGE_NAT_ICMP_IDLE_MS;
    }
}

void bridge_nat_expire(void)
{
    uint32_t now = sys_now();

    for (unsigned i = 0; i < BRIDGE_NAT_TABLE_SIZE; i++) {
        nat_entry_t *e = &nat_table[i];
        if (!e->in_use) {
            continue;
        }
        if ((uint32_t)(now - e->last_ms) >= nat_idle_ms(e->proto)) {
            e->in_use = false;
            bridge_stats.nat_expired++;
        }
    }
}

unsigned bridge_nat_active(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < BRIDGE_NAT_TABLE_SIZE; i++) {
        if (nat_table[i].in_use) {
            n++;
        }
    }
    return n;
}


/* --------------------------- reading a packet ---------------------------- */
static bool nat_read_u16(struct pbuf *p, uint16_t off, uint16_t *out)
{
    uint8_t b[2];
    if (pbuf_copy_partial(p, b, sizeof(b), off) != sizeof(b)) {
        return false;
    }
    *out = (uint16_t)((b[0] << 8) | b[1]);   /* network order on the wire */
    return true;
}

static bool nat_write_u16(struct pbuf *p, uint16_t off, uint16_t value)
{
    uint8_t b[2];
    b[0] = (uint8_t)(value >> 8);
    b[1] = (uint8_t)value;
    return pbuf_take_at(p, b, sizeof(b), off) == ERR_OK;
}

static bool nat_read_u8(struct pbuf *p, uint16_t off, uint8_t *out)
{
    return pbuf_copy_partial(p, out, 1, off) == 1;
}

/* Offset of the field this NAT keys the mapping on.
 * TCP/UDP: the source port (outbound) or the destination port (inbound).
 * ICMP:    the echo identifier, which is what a reply is matched on. */
static bool nat_port_offset(struct pbuf *p, const struct ip_hdr *iphdr, uint16_t hlen,
                            uint16_t iplen, bool outbound, uint16_t *off)
{
    switch (IPH_PROTO(iphdr)) {
        case IP_PROTO_TCP:
        case IP_PROTO_UDP:
            if (iplen < (uint16_t)(hlen + 4)) {
                return false;               /* truncated transport header */
            }
            *off = outbound ? hlen : (uint16_t)(hlen + 2);
            return true;

        case IP_PROTO_ICMP: {
            uint8_t type;
            if ((iplen < (uint16_t)(hlen + 8)) ||
                !nat_read_u8(p, hlen, &type) ||
                ((type != ICMP_ER) && (type != ICMP_ECHO))) {
                return false;               /* no identifier to key on */
            }
            *off = (uint16_t)(hlen + 4);
            return true;
        }

        default:
            return false;                   /* cannot translate this protocol */
    }
}

/* ------------------------------- outbound -------------------------------- */
static bool nat_snat(struct pbuf *p, struct ip_hdr *iphdr, uint16_t hlen, uint16_t iplen)
{
    struct netif *wn = bridge_wifi_netif();
    if (wn == NULL) {
        return false;
    }

    /* A continuation fragment carries no transport header, so there is no port
     * to key on.  Rewrite the address only; fragmented datagrams are rare here
     * and are documented as a limitation rather than silently dropped. */
    if ((lwip_ntohs(IPH_OFFSET(iphdr)) & IP_OFFMASK) != 0) {
        iphdr->src.addr = ip4_addr_get_u32(netif_ip4_addr(wn));
        bridge_stats.nat_out++;
        return true;
    }

    uint16_t port_off;
    if (!nat_port_offset(p, iphdr, hlen, iplen, true, &port_off)) {
        return false;
    }

    uint16_t int_port;
    if (!nat_read_u16(p, port_off, &int_port)) {
        return false;
    }
    int_port = lwip_ntohs(int_port);

    /* struct ip_hdr holds the addresses as packed u32s in the same layout an
     * ip4_addr_t uses; move them by value instead of taking their address. */
    ip4_addr_t int_ip;
    ip4_addr_set_u32(&int_ip, iphdr->src.addr);

    nat_entry_t *e = nat_find_out(IPH_PROTO(iphdr), &int_ip, int_port);
    if (e == NULL) {
        e = nat_create(IPH_PROTO(iphdr), &int_ip, int_port);
        if (e == NULL) {
            return false;                   /* table full */
        }
    }
    e->last_ms = sys_now();

    if (!nat_write_u16(p, port_off, lwip_htons(e->ext_port))) {
        return false;
    }
    iphdr->src.addr = ip4_addr_get_u32(netif_ip4_addr(wn));

    bridge_stats.nat_out++;
    return true;
}

err_t bridge_nat_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr)
{
    struct netif *wn = bridge_wifi_netif();
    struct ip_hdr *iphdr = (struct ip_hdr *)p->payload;
    uint16_t hlen = IPH_HL_BYTES(iphdr);
    uint16_t iplen = lwip_ntohs(IPH_LEN(iphdr));
    ip4_addr_t src;
    bool from_host = false;

    ip4_addr_set_u32(&src, iphdr->src.addr);

    if ((netif == wn) && bridge_usb_ready() &&
        (hlen >= IP_HLEN) && (hlen <= p->len) &&
        (iplen >= hlen) && (iplen <= p->tot_len) &&
        ip4_addr_net_eq(&src, netif_ip4_addr(&bridge_usb_netif),
                        netif_ip4_netmask(&bridge_usb_netif))) {
        from_host = true;
        if (!nat_snat(p, iphdr, hlen, iplen)) {
            /* Drop the frame, but do NOT free p.
             *
             * The caller that installed this function as netif->output is
             * ip4_forward(), and its caller ip4_input() frees the pbuf
             * unconditionally as soon as ip4_forward() returns
             * (core/ipv4/ip4.c: "packet not for us, route or discard" ->
             * ip4_forward(p, ...); pbuf_free(p); return ERR_OK;).
             * ip4_forward() also discards the value netif->output() returns, so
             * ERR_MEM is enough to stop the frame.
             *
             * Freeing p here was a second free of the same pbuf.  It tripped
             * LWIP_ASSERT("pbuf_free: p->ref > 0") in pbuf_free(), which this
             * build routes to diagnostics_lwip_assert(): every frame that took
             * this path cost a warm reboot and a counted fault, with faultpc
             * pointing into pbuf_free().
             *
             * nat_snat() fails on a full BRIDGE_NAT_TABLE_SIZE, on a protocol
             * with no port to key on, and on a truncated transport header - all
             * reachable from ordinary host traffic. */
            bridge_stats.wifi_drop++;
            return ERR_MEM;
        }
    }

    /* Recompute after the rewrite.  A frame reaching this point may have had its
     * checksums computed by lwIP and then invalidated by nat_snat(), or may have
     * arrived with them zeroed by ip4_forward(); bridge_checksum_fixup() handles
     * both, because it clears each field before recomputing it. */
    bridge_checksum_fixup(p);

    /* etharp_output() owns the pbuf either way: it queues it, transmits it or
     * frees it. */
    err_t err = etharp_output(netif, p, ipaddr);
    if (err == ERR_OK) {
        bridge_stats.wifi_tx++;
        /* ->wifi counts the frames that crossed the bridge, not the Pico's own
         * DHCP client, which also leaves through this netif. */
        if (from_host) {
            bridge_stats.to_wifi++;
#if TRACE_FRAMES
            bridge_trace_frame("->wifi", &src, ipaddr, IPH_PROTO(iphdr), iplen);
#endif
        }
    } else {
        bridge_stats.wifi_drop++;
    }
    return err;
}

/* -------------------------------- inbound -------------------------------- */
bool bridge_nat_unnat(struct netif *inp, struct pbuf *p, struct ip_hdr *iphdr)
{
    (void)inp;

    uint16_t hlen = IPH_HL_BYTES(iphdr);
    uint16_t iplen = lwip_ntohs(IPH_LEN(iphdr));

    if ((hlen < IP_HLEN) || (hlen > p->len) ||
        (iplen < hlen) || (iplen > p->tot_len)) {
        return false;
    }
    /* Leave fragments to ip4_reass() - only whole packets are keyed. */
    if ((lwip_ntohs(IPH_OFFSET(iphdr)) & IP_OFFMASK) != 0) {
        return false;
    }
    if (!bridge_usb_ready()) {
        return false;
    }

    uint16_t port_off;
    if (!nat_port_offset(p, iphdr, hlen, iplen, false, &port_off)) {
        return false;
    }

    uint16_t ext_port;
    if (!nat_read_u16(p, port_off, &ext_port)) {
        return false;
    }
    ext_port = lwip_ntohs(ext_port);

    nat_entry_t *e = nat_find_in(IPH_PROTO(iphdr), ext_port);
    if (e == NULL) {
        return false;                       /* nobody behind us asked for this */
    }

    if (!nat_write_u16(p, port_off, lwip_htons(e->int_port))) {
        return false;
    }
    iphdr->dest.addr = ip4_addr_get_u32(&e->int_ip);
    e->last_ms = sys_now();
    bridge_stats.nat_in++;

    /* ip4_input() is about to verify the header checksum, and ip4_forward()
     * will zero the transport checksum for our output path to redo. */
    bridge_checksum_fixup(p);
    return true;
}

#endif /* BRIDGE_NAT_ENABLE */


