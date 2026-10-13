/* ---------------------------------------------------------------------------
 * bridge.c - one lwIP stack, two network interfaces.
 *
 *   w0 - CYW43 Wi-Fi station, on your Wi-Fi network
 *   u1 - TinyUSB CDC-NCM, the interface the USB host sees
 *
 * Two data paths live in this file and src/config.h picks one at build time:
 *
 *   BRIDGE_L2_BRIDGE 0 - lwIP terminates IP on both interfaces and routes
 *   between them (IP_FORWARD + LWIP_HOOK_IP4_ROUTE_SRC + a private network
 *   leased to the host + source NAT out of w0).  See nat.c and bridge_hooks.h.
 *
 *   BRIDGE_L2_BRIDGE 1 - the Pico forwards Ethernet frames: what a frame from
 *   the host looks like on the way in is what leaves on w0, and what the access
 *   point delivers goes straight back out to the host.  The host adopts the
 *   station's MAC and takes the router's own lease; the Pico holds no address.
 *   That path is the "bridge_l2_*" section below and it uses none of the hooks,
 *   none of nat.c and no DHCP server.
 * ------------------------------------------------------------------------ */
#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "tusb.h"

#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/ip4.h"
#include "lwip/ip4_addr.h"
#include "lwip/inet_chksum.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ethernet.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"
#include "lwip/prot/udp.h"
#include "lwip/timeouts.h"
#include "netif/etharp.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

#include "dhserver.h"

#include "bridge.h"
#include "bridge_hooks.h"
#include "config.h"
#include "console.h"
#include "diagnostics.h"
#include "nat.h"

#define DHCP_SERVER_PORT  67

struct netif bridge_usb_netif;
bridge_stats_t bridge_stats;

/* MAC that TinyUSB reports to the host through the iMACAddress descriptor. */
uint8_t tud_network_mac_address[6];

static bool bridge_initialised;
static bool usb_link_up;
static bool wifi_present;         /* cyw43 initialised and netif "w0" exists */

/* Address observed on the frames the USB host sends (see bridge_hook_ip4_input). */
static ip4_addr_t host_ipv4;
static bool host_ipv4_seen;

/* ---------------------------- USB -> lwIP queue -------------------------- */
typedef struct {
    uint16_t len;
    uint8_t  data[BRIDGE_MAX_FRAME];
} frame_slot_t;

static frame_slot_t rx_ring[BRIDGE_USB_RX_SLOTS];
static uint8_t rx_write;
static uint8_t rx_read;

static uint8_t rx_next(uint8_t i)
{
    return (uint8_t)((i + 1u) % BRIDGE_USB_RX_SLOTS);
}

/* ------------------------------ interfaces ------------------------------- */
struct netif *bridge_wifi_netif(void)
{
    return wifi_present ? &cyw43_state.netif[CYW43_ITF_STA] : NULL;
}

bool bridge_wifi_ready(void)
{
    struct netif *wn = bridge_wifi_netif();

    if (wn == NULL) {
        return false;
    }
#if BRIDGE_L2_BRIDGE
    /* A bridged station is ready when it is associated.  It has no address
     * because the bridge holds none - the address on the network belongs to the
     * host - so asking w0 for an IP address here would report "not ready" on a
     * bridge that is working perfectly. */
    return netif_is_up(wn) && netif_is_link_up(wn);
#else
    return netif_is_up(wn) && netif_is_link_up(wn) &&
           !ip4_addr_isany_val(*netif_ip4_addr(wn));
#endif
}

bool bridge_usb_ready(void)
{
    return netif_is_up(&bridge_usb_netif) && usb_link_up;
}

bool bridge_wifi_present(void)
{
    return wifi_present;
}

/* Depth of the USB -> lwIP ring, in frames.  This is the only queue the bridge
 * has today, so it is what ringpk reports (see bridge_format_stats()). */
static unsigned ring_depth(void)
{
    return (unsigned)((rx_write + BRIDGE_USB_RX_SLOTS - rx_read) % BRIDGE_USB_RX_SLOTS);
}

static err_t usb_netif_init(struct netif *netif)
{
    netif->hostname = "pico2";
    netif->mtu      = BRIDGE_HOST_MTU;
    netif->flags   |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;

    /* The hardware address, not just the flags: etharp_output() and
     * ethernet_output() assert netif->hwaddr_len == 6 on an Ethernet netif, and
     * netif_add() leaves it at 0.  Without these six bytes the first frame lwIP
     * tries to send to the host faults the CPU - which is what a board that
     * enumerates and then goes silent actually was.  The address is the one the
     * USB side already advertises, so host and netif agree on it by construction. */
    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, tud_network_mac_address, ETH_HWADDR_LEN);
    return ERR_OK;
}

/* One line per bridged frame, on the debug stream.  The specification asks for
 * this as a build option rather than a command because under load it floods the
 * console; with TRACE_FRAMES=0 the body is empty and nothing is formatted. */
void bridge_trace_frame(const char *dir, const ip4_addr_t *src, const ip4_addr_t *dest,
                       uint8_t proto, uint16_t len)
{
#if TRACE_FRAMES
    char s[16];
    char dst[16];

    console_debug_logf("frame %s  %s -> %s  proto %u  len %u",
                       dir,
                       ip4addr_ntoa_r(src, s, sizeof(s)),
                       ip4addr_ntoa_r(dest, dst, sizeof(dst)),
                       (unsigned)proto, (unsigned)len);

#else
    (void)dir;
    (void)src;
    (void)dest;
    (void)proto;
    (void)len;
#endif
}

/* Raw bytes of a bridged frame, Ethernet header first.  Addresses were not
 * enough: the host receives these frames and refuses them, so the header itself
 * - destination MAC, ethertype, IP length, protocol, checksum - has to be read.
 * First 42 bytes: 14 of Ethernet, 20 of IP, 8 of TCP/UDP. */
#if TRACE_FRAMES
static void bridge_trace_hex(const char *dir, const uint8_t *d, uint16_t len)
{
    static const char hexc[] = "0123456789abcdef";
    char hex[2 * 42 + 1];
    uint16_t n = (len > 42u) ? 42u : len;

    if ((dir == NULL) || (d == NULL)) {
        return;
    }
    for (uint16_t i = 0u; i < n; i++) {
        hex[2 * i]     = hexc[(d[i] >> 4) & 0x0fu];
        hex[2 * i + 1] = hexc[d[i] & 0x0fu];
    }
    hex[2 * n] = '\0';
    console_debug_logf("raw %s  len %u  %s", dir, (unsigned)len, hex);
}
#endif

/* lwIP -> USB host. */
static err_t usb_netif_linkoutput(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    if (!usb_link_up || !tud_network_can_xmit(p->tot_len)) {
        bridge_stats.usb_tx_drop++;
        return ERR_BUF;
    }

#if TRACE_FRAMES
    {
        uint8_t head[42];
        uint16_t n = (p->tot_len > sizeof(head)) ? (uint16_t)sizeof(head)
                                                   : (uint16_t)p->tot_len;

        if (pbuf_copy_partial(p, head, (u16_t)n, 0) == (u16_t)n) {
            bridge_trace_hex("->host", head, n);
        }
    }
#endif

    tud_network_xmit(p, 0);
    bridge_stats.usb_tx++;
    return ERR_OK;
}

/* netif->output for the USB side.  Every frame leaving here gets its checksums
 * recomputed, because the NAT and the forwarding path rewrite addresses on the
 * way out, and lwIP's own transmit path has already filled the fields in - a
 * rewrite invalidates them.  See bridge_checksum_fixup() for the ordering rule. */
static err_t usb_netif_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr)
{
    bridge_checksum_fixup(p);
    return etharp_output(netif, p, ipaddr);
}

static void usb_netif_link_cb(struct netif *netif)
{
    printf("[bridge] usb link %s\n", netif_is_link_up(netif) ? "up" : "down");
}

/* --------------------------- TinyUSB callbacks --------------------------- */
bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    if ((size == 0) || (size > BRIDGE_MAX_FRAME)) {
        bridge_stats.usb_rx_drop++;
        return false;
    }
    uint8_t next = rx_next(rx_write);
    if (next == rx_read) {                 /* ring full, refuse the frame */
        bridge_stats.usb_rx_drop++;
        return false;
    }
    memcpy(rx_ring[rx_write].data, src, size);
    rx_ring[rx_write].len = size;
    rx_write = next;
    bridge_stats.usb_rx++;

    /* ringpk: the deepest the queue got since the last stats line.  A number
     * close to BRIDGE_USB_RX_SLOTS means the host is sending faster than lwIP
     * can take the frames. */
    unsigned depth = ring_depth();
    if (depth > bridge_stats.ringpk) {
        bridge_stats.ringpk = (uint16_t)depth;
    }
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
    struct pbuf *p = (struct pbuf *)ref;
    (void)arg;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

void tud_network_init_cb(void)
{
}

void tud_mount_cb(void)
{
    usb_link_up = true;
    netif_set_link_up(&bridge_usb_netif);
}

void tud_umount_cb(void)
{
    usb_link_up = false;
    netif_set_link_down(&bridge_usb_netif);
}

/* ---------------------------- checksum repair ---------------------------- */
/*
 * ip4_forward() zeroes the IPv4 header checksum and the transport checksum
 * before handing the packet to netif->output, because lwIP's own transmit path
 * (ip4_output_if_src) recomputes them.  A forwarded packet never goes through
 * that path, so every output side of this bridge rebuilds them here.
 *
 * The walk below mirrors inet_cksum_pseudo_base() in lwIP's inet_chksum.c, but
 * starts at an arbitrary offset so it can be used while the IP header is still
 * attached to the pbuf.
 */
/* lwIP's portable checksum.  Its prototype lives inside inet_chksum.c rather
 * than in a public header, so declare it the way lwIP's own code does. */
#ifndef LWIP_CHKSUM
u16_t lwip_standard_chksum(const void *dataptr, int len);
#define LWIP_CHKSUM  lwip_standard_chksum
#endif

static u16_t cksum_transport(struct pbuf *p, u16_t offset, u16_t len, u8_t proto,
                             const ip4_addr_t *src, const ip4_addr_t *dest, bool pseudo)
{
    u32_t acc = 0;
    int swapped = 0;
    u16_t remaining = len;
    size_t skip = offset;

    if (pseudo) {
        u32_t addr = ip4_addr_get_u32(src);
        acc = (addr & 0xffffUL) + ((addr >> 16) & 0xffffUL);
        addr = ip4_addr_get_u32(dest);
        acc = acc + (addr & 0xffffUL) + ((addr >> 16) & 0xffffUL);
        acc = FOLD_U32T(acc);
        acc = FOLD_U32T(acc);
    }

    for (struct pbuf *q = p; (q != NULL) && (remaining > 0); q = q->next) {
        if (skip >= q->len) {
            skip -= q->len;
            continue;
        }
        u16_t chunk = (u16_t)(q->len - skip);
        if (chunk > remaining) {
            chunk = remaining;
        }
        acc += LWIP_CHKSUM((const u8_t *)q->payload + skip, chunk);
        remaining = (u16_t)(remaining - chunk);
        acc = FOLD_U32T(acc);
        /* The byte order of everything after this chunk depends on how many
         * bytes were summed here - chunk, not the whole pbuf, since the chunk
         * may start partway into q and may stop short of its end. */
        if ((chunk & 1u) != 0) {
            swapped = !swapped;
            acc = SWAP_BYTES_IN_WORD(acc);
        }
        skip = 0;
    }

    if (swapped) {
        acc = SWAP_BYTES_IN_WORD(acc);
    }

    if (pseudo) {
        acc += (u32_t)lwip_htons((u16_t)proto);
        acc += (u32_t)lwip_htons(len);
        acc = FOLD_U32T(acc);
        acc = FOLD_U32T(acc);
    }

    return (u16_t)~(acc & 0xffffUL);
}

void bridge_checksum_fixup(struct pbuf *p)
{
    struct ip_hdr *iphdr = (struct ip_hdr *)p->payload;
    u16_t hlen = IPH_HL_BYTES(iphdr);
    u16_t iplen = lwip_ntohs(IPH_LEN(iphdr));

    if ((hlen < IP_HLEN) || (hlen > p->len) || (iplen < hlen) || (iplen > p->tot_len)) {
        return;                     /* not something we are willing to touch */
    }

    /* The internet checksum is self-verifying: summing a header that already
     * carries a correct checksum folds to zero, so recomputing over a header
     * that is already correct yields 0 and blanks the field.  The field has to
     * be cleared first - lwIP's own transmit path does exactly this
     * (ip4_output_if_src() sets IPH_CHKSUM to 0 before calling inet_chksum()).
     * Skipping that step is what made every frame lwIP built itself - the DHCP
     * server's OFFERs above all - leave with a zero IP checksum, which every
     * host on the other end of the USB link discards. */
    IPH_CHKSUM_SET(iphdr, 0);
    IPH_CHKSUM_SET(iphdr, inet_chksum(p->payload, hlen));

    /* struct ip_hdr keeps the addresses as packed u32s laid out exactly like an
     * ip4_addr_t, so copy them by value rather than borrowing a pointer to a
     * member that need not be aligned. */
    ip4_addr_t src, dest;
    ip4_addr_set_u32(&src, iphdr->src.addr);
    ip4_addr_set_u32(&dest, iphdr->dest.addr);

    u16_t tlen = (u16_t)(iplen - hlen);
    if (tlen == 0) {
        return;
    }

    switch (IPH_PROTO(iphdr)) {
        case IP_PROTO_TCP: {
            if ((u16_t)(hlen + TCP_HLEN) > p->len) {
                break;              /* transport header split across pbufs */
            }
            struct tcp_hdr *tcphdr = (struct tcp_hdr *)((u8_t *)p->payload + hlen);
            /* Same rule as for the IP header: clear, then recompute. */
            tcphdr->chksum = 0;
            /* lwIP's *_chksum_pseudo() helpers return the value to store
             * straight into the header, network order included. */
            tcphdr->chksum = cksum_transport(p, hlen, tlen, IP_PROTO_TCP,
                                             &src, &dest, true);
            break;
        }
        case IP_PROTO_UDP: {
            if ((u16_t)(hlen + UDP_HLEN) > p->len) {
                break;              /* transport header split across pbufs */
            }
            struct udp_hdr *udphdr = (struct udp_hdr *)((u8_t *)p->payload + hlen);
            udphdr->chksum = 0;
            u16_t sum = cksum_transport(p, hlen, tlen, IP_PROTO_UDP,
                                        &src, &dest, true);
            /* A zero UDP checksum means "unused", so a computed zero has to be
             * sent as 0xffff instead. */
            udphdr->chksum = (sum == 0) ? 0xffff : sum;
            break;
        }
        case IP_PROTO_ICMP: {
            if ((u16_t)(hlen + 4) > p->len) {
                break;              /* the ICMP checksum field is at offset 2 */
            }
            struct icmp_hdr *icmphdr = (struct icmp_hdr *)((u8_t *)p->payload + hlen);
            icmphdr->chksum = 0;
            icmphdr->chksum = cksum_transport(p, hlen, tlen, IP_PROTO_ICMP,
                                              NULL, NULL, false);
            break;
        }
        default:
            break;
    }
}



/* ------------------------------ lwIP hooks ------------------------------- */
/*
 * These two exist only for the L3 path.  When the Pico forwards Ethernet frames
 * there is no IP routing decision to override, so lwipopts.h does not even set
 * LWIP_HOOK_FILENAME and lwIP never calls them; keeping them out of the build
 * as well keeps the image honest about which bridge it contains.
 */
#if !BRIDGE_L2_BRIDGE
/*
 * LWIP_HOOK_IP4_ROUTE_SRC is asked "which interface does a packet with this
 * source address leave on?".  Returning NULL lets lwIP fall back to its own
 * idea, which is simply the default netif (the Wi-Fi one, set by cyw43).
 */
struct netif *bridge_hook_route_src(const ip4_addr_t *src, const ip4_addr_t *dest)
{
    if (dest == NULL) {
        return NULL;
    }

    if (bridge_usb_ready()) {
        const ip4_addr_t *me   = netif_ip4_addr(&bridge_usb_netif);
        const ip4_addr_t *mask = netif_ip4_netmask(&bridge_usb_netif);

        /* Anything aimed at the USB-side network leaves through the USB port. */
        if (ip4_addr_net_eq(dest, me, mask)) {
            return &bridge_usb_netif;
        }

        /* Anything sourced from the USB-side network leaves through Wi-Fi. */
        if ((src != NULL) && ip4_addr_net_eq(src, me, mask)) {
            if (bridge_wifi_ready()) {
                bridge_stats.forwarded++;
                return bridge_wifi_netif();
            }
            /* No uplink: the host is talking into a dead end. */
            bridge_stats.no_route++;
            return NULL;
        }

        /*
         * TinyUSB's DHCP server sends its OFFER/ACK to 255.255.255.255 from a
         * pcb bound to 0.0.0.0.  A limited broadcast matches no netif, so
         * lwIP would use netif_default - the Wi-Fi interface - and the reply
         * would vanish.  Nothing else on this device broadcasts, so pin that
         * case to the USB interface.
         */
        if (((src == NULL) || ip4_addr_isany_val(*src)) &&
            (ip4_addr_isany_val(*dest) || ip4_addr_eq(dest, IP4_ADDR_BROADCAST))) {
            return &bridge_usb_netif;
        }
    }

    return NULL;
}

/*
 * LWIP_HOOK_IP4_INPUT runs in ip4_input() before lwIP works out whether the
 * packet is meant for this device.  Undoing NAT here turns a reply aimed at the
 * Wi-Fi address into one aimed at the USB host, which lwIP then forwards as if
 * it had always been a packet passing through.
 *
 * The hook keeps its hands off the pbuf except where it says so: returning
 * non-zero tells lwIP the packet has been eaten, lwIP stops processing it and
 * does not free it either, so a hook that returns 1 must have freed it first.
 */
int bridge_hook_ip4_input(struct pbuf *p, struct netif *inp)
{
    if (inp == &bridge_usb_netif) {
        /* Passive snooping: the address the host actually uses is the source
         * address of the frames it sends.  The console reports that, because a
         * bridge has no address of its own to report. */
        if (p->len >= IP_HLEN) {
            struct ip_hdr *iphdr = (struct ip_hdr *)p->payload;
            ip4_addr_t src;
            ip4_addr_set_u32(&src, iphdr->src.addr);
            if (!ip4_addr_isany_val(src) &&
                !ip4_addr_eq(&src, netif_ip4_addr(&bridge_usb_netif))) {
                host_ipv4 = src;
                host_ipv4_seen = true;
            }
        }
        return 0;
    }
    if (!bridge_usb_ready()) {
        return 0;
    }
    if (p->len < IP_HLEN) {
        return 0;
    }

    /* refl: a frame that arrived on Wi-Fi carrying an address from the USB-side
     * network can only be the access point bouncing the host's own transmission
     * back.  Forwarding it would push it straight out the way it came, so it is
     * counted and dropped. */
    /* refl: a frame that arrived on Wi-Fi carrying an address from the USB-side
     * network can only be the access point bouncing the host's own transmission
     * back.  Forwarding it would push it straight out the way it came, so it is
     * counted and dropped. */
    {
        struct ip_hdr *iphdr = (struct ip_hdr *)p->payload;
        ip4_addr_t src;

        ip4_addr_set_u32(&src, iphdr->src.addr);
        if (ip4_addr_net_eq(&src, netif_ip4_addr(&bridge_usb_netif),
                            netif_ip4_netmask(&bridge_usb_netif))) {
            bridge_stats.refl++;
            pbuf_free(p);           /* returning non-zero claims the pbuf */
            return 1;
        }
    }

    bridge_nat_unnat(inp, p, (struct ip_hdr *)p->payload);
    return 0;
}
#endif /* !BRIDGE_L2_BRIDGE: the hooks belong to the L3 path only */


/* ------------------------- transparent L2 forwarding ----------------------- */
#if BRIDGE_L2_BRIDGE

/* The address the host actually uses, read off the frames it sends.  In this
 * build the Pico holds no address of its own, so this is the only address the
 * console can honestly report (see bridge_host_ipv4_word()). */
static void bridge_l2_snoop_host(struct pbuf *p)
{
    uint8_t raw[SIZEOF_ETH_HDR + IP_HLEN];

    if (p->tot_len < (SIZEOF_ETH_HDR + IP_HLEN)) {
        return;                     /* ARP, IPv6, mDNS: no IPv4 address to read */
    }
    if (pbuf_copy_partial(p, raw, (u16_t)sizeof(raw), 0) !=
        (u16_t)(SIZEOF_ETH_HDR + IP_HLEN)) {
        return;
    }
    if (((uint16_t)((raw[12] << 8) | raw[13])) != ETHTYPE_IP) {
        return;
    }

    /* struct ip_hdr keeps its addresses packed; read them octet by octet and
     * rebuild one with IP4_ADDR() rather than taking the address of a member. */
    ip4_addr_t src;
    IP4_ADDR(&src, raw[SIZEOF_ETH_HDR + 12], raw[SIZEOF_ETH_HDR + 13],
             raw[SIZEOF_ETH_HDR + 14], raw[SIZEOF_ETH_HDR + 15]);
    if (!ip4_addr_isany_val(src)) {
        host_ipv4 = src;
        host_ipv4_seen = true;
    }
}
/*
 * One Ethernet frame in, one Ethernet frame out on the other interface.
 *
 * There is no IP decision here and nothing that can run out: no routing table,
 * no address translation, no table of flows.  A frame from the host leaves on
 * the Wi-Fi interface with its source MAC unchanged - which is the whole point
 * of the host adopting the station's MAC - and a frame the access point
 * delivers goes back out to the host unchanged.  The host is then a station on
 * your Wi-Fi network with the router's own address, not a host behind the Pico.
 *
 * Ownership, because this is where a double free would hide:
 *   - a pbuf passed to netif->linkoutput() is still ours afterwards (lwIP's own
 *     callers, etharp_output() and ethernet_output(), free it themselves), so
 *     every branch below frees it exactly once;
 *     lwIP's ethernet_input() would have consumed one passed to it, which is
 *     why these input functions return ERR_OK after freeing rather than letting
 *     the caller free it a second time.
 */
static void bridge_l2_forward(struct netif *from, struct pbuf *p)
{
    struct netif *wn = bridge_wifi_netif();
    struct netif *to;

    if (p->tot_len < SIZEOF_ETH_HDR) {
        bridge_stats.usb_rx_drop++;      /* not an Ethernet frame: refuse it */
        pbuf_free(p);
        return;
    }

    if (wn == NULL) {
        bridge_stats.no_route++;
        pbuf_free(p);
        return;
    }

    if (from == &bridge_usb_netif) {
        /* txdrop, in the sense table 2 uses it: the host sent a frame and the
         * station was not associated yet, so there is nowhere for it to go.
         * The host retransmits; nothing here has to remember anything. */
        if (!netif_is_link_up(wn)) {
            bridge_stats.no_route++;
            pbuf_free(p);
            return;
        }
        to = wn;
    } else {
        if (!bridge_usb_ready()) {
            bridge_stats.usb_tx_drop++;
            pbuf_free(p);
            return;
        }
        to = &bridge_usb_netif;
    }

    if (to->linkoutput == NULL) {
        if (to == wn) {
            bridge_stats.wifi_drop++;
        } else {
            bridge_stats.usb_tx_drop++;
        }
        pbuf_free(p);
        return;
    }

    err_t err = to->linkoutput(to, p);
    if (err == ERR_OK) {
        if (to == wn) {
            bridge_stats.to_wifi++;
        }
    } else if (to == wn) {
        /* Only the Wi-Fi side is counted here.  usb_netif_linkoutput() already
         * counted its own refusal into usb_tx_drop, and cyw43_netif_output()
         * counts nothing, so counting the USB failure here too would report one
         * lost frame twice. */
        bridge_stats.wifi_drop++;
    }

    pbuf_free(p);                  /* ours to free: linkoutput() did not take it */
}

/* netif->input on the USB port, replacing ethernet_input() for this build. */
static err_t bridge_l2_usb_input(struct pbuf *p, struct netif *netif)
{
    bridge_l2_snoop_host(p);
#if TRACE_FRAMES
    {
        uint8_t head[42];
        uint16_t n = (p->tot_len > sizeof(head)) ? (uint16_t)sizeof(head)
                                                   : (uint16_t)p->tot_len;
        if (pbuf_copy_partial(p, head, (u16_t)n, 0) == (u16_t)n) {
            bridge_trace_hex("->wifi", head, n);
        }
    }
#endif
    bridge_l2_forward(netif, p);
    return ERR_OK;                 /* we consumed p; the caller must not free it */
}

/* netif->input on the Wi-Fi port, replacing the ethernet_input() cyw43 sets. */
static err_t bridge_l2_wifi_input(struct pbuf *p, struct netif *netif)
{
    if (p->tot_len < SIZEOF_ETH_HDR) {
        pbuf_free(p);
        return ERR_OK;
    }

    struct eth_addr *src = (struct eth_addr *)(((u8_t *)p->payload) + ETH_HWADDR_LEN);

    /* refl: with the host using the station's own MAC, any frame arriving on
     * Wi-Fi *sourced* by that MAC can only be the access point bouncing the
     * host's own transmission back.  Sending it out again would push it back
     * the way it came, so it is counted and dropped. */
    if (memcmp(src, netif->hwaddr, ETH_HWADDR_LEN) == 0) {
        bridge_stats.refl++;
        pbuf_free(p);
        return ERR_OK;
    }

    bridge_l2_forward(netif, p);
    return ERR_OK;                 /* cyw43 frees only if we return non-ERR_OK */
}
#endif /* BRIDGE_L2_BRIDGE */


/* ----------------------- USB-side DHCP (TinyUSB dhserver) ---------------- */
/* This whole block belongs to the L3 path.  A transparent bridge does not lease
 * addresses: the host takes its lease from your router, exactly as any other
 * station on the network does, so there is no server to run and no pool to
 * exhaust. */
#if !BRIDGE_L2_BRIDGE
/* dhserver.h is TinyUSB's own miniature lwIP-based DHCP server.  It answers on
 * netif_default, so bridge_hook_route_src() has to keep netif_default pointing
 * at the Wi-Fi interface only for traffic that really belongs there. */
static dhcp_entry_t dhcp_entries[BRIDGE_HOST_POOL_SIZE];
static dhcp_config_t  dhcp_config;

static void bridge_init_dhcp(void)
{
    for (int i = 0; i < BRIDGE_HOST_POOL_SIZE; i++) {
        memset(dhcp_entries[i].mac, 0, sizeof(dhcp_entries[i].mac));
        BRIDGE_IP4_ADDR(&dhcp_entries[i].addr, BRIDGE_HOST_NET_PREFIX, BRIDGE_HOST_POOL_FIRST + i);
        dhcp_entries[i].lease = BRIDGE_HOST_LEASE_SECONDS;
    }

    memset(&dhcp_config, 0, sizeof(dhcp_config));
    dhcp_config.port      = DHCP_SERVER_PORT;
    dhcp_config.num_entry = BRIDGE_HOST_POOL_SIZE;
    dhcp_config.entries   = dhcp_entries;
    dhcp_config.domain    = BRIDGE_HOST_DOMAIN;
    BRIDGE_IP4_ADDR(&dhcp_config.router, BRIDGE_HOST_IP);
    BRIDGE_IP4_ADDR(&dhcp_config.dns, BRIDGE_FALLBACK_DNS);
}

/* Once the Wi-Fi side has been told a resolver by its own DHCP server, pass
 * that one on to the USB host instead of the hard-coded fallback.
 *
 * The only call site is bridge_service_wifi(), and that exists only when
 * BRIDGE_WIFI_ENABLE is 1; defining this outside that guard left an unused
 * static function in the USB-only build. */
#if BRIDGE_WIFI_ENABLE
static void bridge_refresh_dns(void)
{
#if LWIP_DNS
    const ip_addr_t *learned = dns_getserver(0);
    if ((learned != NULL) && !ip4_addr_isany_val(*ip_2_ip4(learned))) {
        ip4_addr_copy(dhcp_config.dns, *ip_2_ip4(learned));
    }
#endif
}
#endif /* BRIDGE_WIFI_ENABLE */
#else  /* BRIDGE_L2_BRIDGE: no server, no pool, nothing to refresh. */
#if BRIDGE_WIFI_ENABLE
static void bridge_refresh_dns(void)
{
}
#endif /* BRIDGE_WIFI_ENABLE */
#endif /* !BRIDGE_L2_BRIDGE */

/* ------------------------------ Wi-Fi uplink ----------------------------- */
#if BRIDGE_L2_BRIDGE
/* Defined below, next to the forwarder it installs.  Declared here because
 * bridge_service_wifi() has to be able to re-run it. */
static void bridge_attach_l2_ports(void);
#endif

#if BRIDGE_WIFI_ENABLE
static bool     wifi_connecting;      /* an async connect attempt is live */
static absolute_time_t wifi_retry_at;
static absolute_time_t wifi_giveup_at;

/* A profile with a passphrase joins in WPA2/WPA3 transition mode, which is what
 * lets one profile work against a WPA2-PSK or a WPA3-SAE access point.  An
 * empty passphrase is an open network.  Override with -DBRIDGE_WIFI_AUTH to pin
 * one mode. */
#ifndef BRIDGE_WIFI_AUTH_TRANSITION
#define BRIDGE_WIFI_AUTH_TRANSITION 1
#endif

static uint32_t bridge_profile_auth(const wifi_profile_t *p)
{
    if ((p == NULL) || (p->pass[0] == '\0')) {
        return CYW43_AUTH_OPEN;
    }
#if BRIDGE_WIFI_AUTH_TRANSITION
    return CYW43_AUTH_WPA3_WPA2_AES_PSK;
#else
    return BRIDGE_WIFI_AUTH;
#endif
}

static void bridge_service_wifi(void)
{
    if (!wifi_present) {
        return;
    }

    struct netif *wn = bridge_wifi_netif();

#if BRIDGE_L2_BRIDGE
    /* cyw43 re-adds w0 (cyw43_cb_tcpip_init, cyw43_ctrl.c:567-568) whenever the
     * interface is brought up again, and that puts ethernet_input() back on the
     * bridged port and restarts the station's own DHCP client.  Both are wrong
     * for this build, so the attachment is re-checked every poll instead of
     * assumed to survive the run. */
    bridge_attach_l2_ports();
#endif

    (void)wn;                     /* only the L3 path prints the address */
    if (bridge_wifi_ready()) {
        if (wifi_connecting) {
            wifi_connecting = false;
#if BRIDGE_L2_BRIDGE
            printf("[bridge] wifi up (bridged, no address of its own)\n");
#else
            printf("[bridge] wifi up: %s\n", ip4addr_ntoa(netif_ip4_addr(wn)));
#endif
            bridge_refresh_dns();
            /* The association event the debug stream exists to report. */
            console_debug_logf("associated to %s",
                               console_active_profile() ? console_active_profile()->ssid : "");
        }
        return;
    }


    absolute_time_t now = get_absolute_time();

    if (wifi_connecting) {
        /* cyw43 is still trying; give it a hard deadline so a dead access
         * point does not leave us stuck in one attempt forever. */
        if (absolute_time_diff_us(now, wifi_giveup_at) <= 0) {
            wifi_connecting = false;
            printf("[bridge] wifi: attempt timed out\n");
            console_debug_logf("association failed: %s", bridge_link_word());
        }
        return;
    }

    if (absolute_time_diff_us(now, wifi_retry_at) > 0) {
        return;
    }

    const wifi_profile_t *prof = console_active_profile();

    if (prof == NULL) {
        /* Nothing provisioned yet: there is no network to join.  The management
         * console is where that gets fixed; until then the bridge just waits. */
        wifi_retry_at = make_timeout_time_ms(BRIDGE_WIFI_RETRY_SECONDS * 1000);
        return;
    }

    printf("[bridge] wifi: connecting to \"%s\"\n", prof->ssid);
    if (cyw43_arch_wifi_connect_async(prof->ssid, prof->pass, bridge_profile_auth(prof)) == 0) {
        wifi_connecting = true;
        wifi_giveup_at = make_timeout_time_ms(20000);
    }
    wifi_retry_at = make_timeout_time_ms(BRIDGE_WIFI_RETRY_SECONDS * 1000);
}

/* Leave the current network and ask for the active profile straight away, so a
 * console change takes effect in the next poll instead of at the next reboot. */
void bridge_wifi_restart(void)
{
    if (!wifi_present) {
        return;
    }
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    wifi_connecting = false;
    wifi_retry_at = get_absolute_time();
}
#endif /* BRIDGE_WIFI_ENABLE */

#if !BRIDGE_WIFI_ENABLE
void bridge_wifi_restart(void)
{
    /* No radio in this build. */
}
#endif /* BRIDGE_WIFI_ENABLE */

/* -------------------------------- diagnostics ---------------------------- */
/* The link word of table 2: up when the station is associated and has an
 * address, join while it is still joining, otherwise the reason it failed. */
const char *bridge_link_word(void)
{
#if BRIDGE_WIFI_ENABLE
    if (!wifi_present) {
        return "fail";
    }

    /* "up" cannot come from cyw43_wifi_link_status(): for the STA interface that
     * function tops out at CYW43_LINK_JOIN - it never returns CYW43_LINK_UP or
     * CYW43_LINK_NOIP (cyw43_ctrl.c:613-630 maps WIFI_JOIN_STATE_ACTIVE straight
     * to CYW43_LINK_JOIN).  Asking it for "up" would leave the console reporting
     * "join" forever, even on a board that is associated and has its lease.  The
     * netif is the thing that actually shows both halves of the requirement - the
     * association completed and the router's address arrived - so "up" comes from
     * there, and the driver is still what tells the failure words apart. */
    if (bridge_wifi_ready()) {
        return "up";
    }
    switch (cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA)) {
        case CYW43_LINK_JOIN:
        case CYW43_LINK_NOIP:
            return "join";
        case CYW43_LINK_BADAUTH:
            return "badauth";
        case CYW43_LINK_NONET:
            return "nonet";
        case CYW43_LINK_FAIL:
            return "fail";
        default:                        /* CYW43_LINK_DOWN and anything else  */
            return bridge_settings.count ? "down" : "fail";
    }
#else
    return "fail";
#endif
}


const char *bridge_status_word(void)
{
    if (!bridge_usb_ready()) {
        return "usb not ready";
    }
    if (!bridge_wifi_present()) {
        return "no radio";
    }
    if (bridge_settings.count == 0u) {
        return "unconfigured";
    }
    return bridge_wifi_ready() ? "associated" : "associating";
}

const char *bridge_host_ipv4_word(void)
{
    static char word[16];

    if (!host_ipv4_seen) {
        return "(not seen yet)";
    }
    return ip4addr_ntoa_r(&host_ipv4, word, sizeof(word));
}

/* The address the router gave the station, and the gateway it learned with it.
 * These are what separate "associated" from "usable": cyw43 reports the link up
 * before its DHCP client has an address, and the console has to say which of the
 * two it is looking at.  Reported as they are - with no radio, or with a link but
 * no lease, the words say so instead of inventing an address. */
const char *bridge_wifi_ipv4_word(void)
{
    static char word[16];
#if BRIDGE_WIFI_ENABLE
#if BRIDGE_L2_BRIDGE
    (void)word;
    /* Reported as it is.  A transparent bridge owns no address: the address on
     * your network belongs to the host, and the console shows it under
     * "host IPv4", snoopated from the frames the host sends. */
    return "(bridge: none)";
#else
    struct netif *wn = bridge_wifi_netif();

    if ((wn == NULL) || ip4_addr_isany_val(*netif_ip4_addr(wn))) {
        return "(no address yet)";
    }
    return ip4addr_ntoa_r(netif_ip4_addr(wn), word, sizeof(word));
#endif
#else
    (void)word;
    return "(no radio)";
#endif
}

const char *bridge_wifi_gateway_word(void)
{
    static char word[16];

#if BRIDGE_WIFI_ENABLE
#if BRIDGE_L2_BRIDGE
    /* Same reason as the address above: a transparent bridge owns no gateway
     * either.  Reporting the gateway the station's own client once learned would
     * describe a route this build does not use, and the station's client is
     * stopped anyway. */
    (void)word;
    return "(bridge: none)";
#else
    struct netif *wn = bridge_wifi_netif();

    if ((wn == NULL) || ip4_addr_isany_val(*netif_ip4_gw(wn))) {
        return "(none)";
    }
    return ip4addr_ntoa_r(netif_ip4_gw(wn), word, sizeof(word));
#endif
#else
    (void)word;
    return "(none)";
#endif
}
/* The two Ethernet addresses a bridge is judged by: the one TinyUSB advertises on
 * the host's interface, and the one the radio owns.  In the transparent bridge
 * they must be the same number - that is what "one MAC, end-to-end identity"
 * means - and the match has to be readable on the console, because the line
 * bridge_init_mac() prints goes to UART0 and nothing is plugged into UART0 here.
 * Formatted octet by octet rather than with a six-argument format string: the
 * order of those six arguments is exactly what a reader has to be able to trust. */
static void bridge_format_mac(const uint8_t *mac, char *word, size_t size)
{
    size_t used = 0u;

    for (size_t i = 0; i < ETH_HWADDR_LEN; i++) {
        if (used + 3u >= size) {
            break;
        }
        used += (size_t)snprintf(word + used, size - used,
                                 (i == 0u) ? "%02x" : ":%02x", mac[i]);
    }
}

const char *bridge_usb_mac_word(void)
{
    static char word[18];

    bridge_format_mac(tud_network_mac_address, word, sizeof(word));
    return word;
}

const char *bridge_station_mac_word(void)
{
    static char word[18];
#if BRIDGE_WIFI_ENABLE
    struct netif *wn = bridge_wifi_netif();

    if ((wn == NULL) || (wn->hwaddr_len != ETH_HWADDR_LEN)) {
        return "(no radio)";
    }
    bridge_format_mac(wn->hwaddr, word, sizeof(word));
    return word;
#else
    (void)word;
    return "(no radio)";
#endif
}




/* -------------------------------- diagnostics ---------------------------- */
/* The onboard LED is a GPIO on the CYW43 chip, so it can only be driven when
 * the radio initialised.  With no radio there is no LED at all, which is why
 * "off = USB not ready" cannot be shown on a board whose radio failed to come
 * up. */
void bridge_set_led(bool on)
{
#if BRIDGE_LED
    if (wifi_present) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on ? 1 : 0);
    }
#else
    (void)on;
#endif
}

#if BRIDGE_LED
/* Double flash: two quick pulses, then a pause. */
static const uint8_t bridge_led_flash[10] = {1u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};

/* The level a named pattern drives on this tick.  Kept separate from the choice
 * of pattern so that a self-test build can be told to show a pattern its state
 * does not call for and the waveform still comes from the same table. */
static bool bridge_led_level_for(const char *pattern, uint8_t tick)
{
    if (strcmp(pattern, "off") == 0) {
        return false;
    }
    if (strcmp(pattern, "solid") == 0) {
        return true;
    }
    if (strcmp(pattern, "double") == 0) {
        return bridge_led_flash[tick] != 0u;
    }
    if (strcmp(pattern, "5hz") == 0) {
        return (tick & 1u) != 0u;
    }
    return tick < 5u;                             /* "1hz" */
}

/* Which of the five patterns of table 3 the current state calls for. */
static const char *bridge_led_pattern_for(void)
{
    if (!bridge_usb_ready()) {
        return "off";                         /* off: USB not ready             */
    }
    if (bridge_wifi_ready()) {
        return "solid";                       /* solid: associated              */
    }
    if (console_scan_live()) {
        return "double";                      /* double flash: live scan        */
    }
    if (bridge_settings.count == 0u) {
        return "5hz";                         /* 5 Hz: nothing configured       */
    }
    return "1hz";                             /* 1 Hz: configured, not up yet   */
}
#endif

#if BRIDGE_SELFTEST
/* What bridge_led_service() decided on its last tick, for `test led`.  A CYW43
 * output pin cannot be read back - cyw43_ll_gpio_get() reads the ioctl
 * "ccgpioin", the radio's input state, not the "gpioout" value that
 * bridge_set_led() wrote - so the chooser is the only place that knows which
 * pattern it picked and at what level.  These live outside #if BRIDGE_LED
 * because the accessors below are compiled with BRIDGE_LED off too, and a
 * static that no accessor can reach would not compile. */
static const char *led_pattern = "off";
static const char *led_state = "off";             /* what the state asked for   */
static bool led_level;
static uint8_t led_tick;
static const char *led_forced;                /* NULL: the state drives it      */
static uint16_t led_force_ticks;              /* ticks left in the forced one   */
#endif

/* The five patterns of table 3.  The onboard LED is a CYW43 GPIO, so with no
 * radio there is nothing to drive: bridge_set_led() says so. */
static void bridge_led_service(void)
{
#if BRIDGE_LED
    static uint8_t tick;                      /* 100 ms ticks, wrapping at 10  */
    const char *chosen;
    const char *pattern;
    bool on;

    tick = (uint8_t)((tick + 1u) % 10u);
    chosen = bridge_led_pattern_for();
    pattern = chosen;

#if BRIDGE_SELFTEST
    if ((led_forced != NULL) && (led_force_ticks > 0u)) {
        pattern = led_forced;                 /* `test led <name>` holds one    */
        led_force_ticks--;
    }
    led_state = chosen;                       /* reported next to what is driven */
    led_pattern = pattern;
    led_tick = tick;
#endif

    on = bridge_led_level_for(pattern, tick);

#if BRIDGE_SELFTEST
    led_level = on;
#endif

    bridge_set_led(on);
#endif
}

#if BRIDGE_SELFTEST
/* The pattern bridge_led_service() showed on its last tick, the level it drove
 * and its place in the ten-tick cycle.  Only `test led` reads them; a build
 * with BRIDGE_SELFTEST=0 does not contain this block at all. */
const char *bridge_led_pattern(void)
{
    if (!bridge_wifi_present()) {
        return "no radio";                    /* bridge_set_led() drives nothing */
    }
    return led_pattern;
}

/* What the state machine asked for on that same tick, which is not the same
 * thing while `test led <name>` is holding a pattern.  Reporting both keeps the
 * chooser observable while its output is overridden. */
const char *bridge_led_state(void)
{
    if (!bridge_wifi_present()) {
        return "no radio";
    }
    return led_state;
}

bool bridge_led_level(void)
{
    return led_level;
}

uint32_t bridge_led_tick(void)
{
    return led_tick;
}

/* Hold one of the five patterns for a while whatever the state says, so each
 * waveform can be measured from the host instead of inferred.  Returns false
 * for a name that is not one of the five. */
bool bridge_led_force(const char *pattern, uint32_t seconds)
{
    static const char *const known[5] = {"off", "solid", "double", "5hz", "1hz"};

    for (unsigned i = 0u; i < 5u; i++) {
        if (strcmp(known[i], pattern) == 0) {
            uint32_t ticks = ((uint32_t)seconds * 1000u + BRIDGE_LED_TICK_MS - 1u)
                             / BRIDGE_LED_TICK_MS;

            led_forced = known[i];
            led_force_ticks = (uint16_t)ticks;
            return true;
        }
    }
    return false;
}
#endif




void bridge_print_status(void)
{
    struct netif *wn = bridge_wifi_netif();
    const bridge_stats_t *s = &bridge_stats;

#if BRIDGE_L2_BRIDGE
    (void)wn;
    printf("[bridge] wifi %-15s usb %-4s  bridged  usb rx/tx %lu/%lu drop %lu/%lu"
           "  wdrop %lu refl %lu noroute %lu\n",
           bridge_wifi_ready() ? "up" : "down",
           bridge_usb_ready() ? "up" : "down",
           (unsigned long)s->usb_rx, (unsigned long)s->usb_tx,
           (unsigned long)s->usb_rx_drop, (unsigned long)s->usb_tx_drop,
           (unsigned long)s->wifi_drop,
           (unsigned long)s->refl, (unsigned long)s->no_route);
#else
    printf("[bridge] wifi %-15s usb %-4s nat %2u/%u  usb rx/tx %lu/%lu drop %lu/%lu"
           "  fwd %lu noroute %lu\n",
           bridge_wifi_ready() ? ip4addr_ntoa(netif_ip4_addr(wn)) : "down",
           bridge_usb_ready() ? "up" : "down",
           bridge_nat_active(), (unsigned)BRIDGE_NAT_TABLE_SIZE,
           (unsigned long)s->usb_rx, (unsigned long)s->usb_tx,
           (unsigned long)s->usb_rx_drop, (unsigned long)s->usb_tx_drop,
           (unsigned long)s->forwarded, (unsigned long)s->no_route);
#endif
}

/* The periodic line of the debug console, field for field as table 2 defines
 * it.  The mapping from the counters above to those fields:
 *
 *   ->wifi   to_wifi    host frames that really left on the Wi-Fi interface
 *            (wifi_tx also counts the Pico's own DHCP client, so it is not it)
 *   ->host   usb_tx     frames handed to the host
 *   txdrop   no_route   host frames with no uplink to go out on
 *   wdrop    wifi_drop  host frames the Wi-Fi side refused after it was chosen
 *            (not in table 2, and it is reported anyway: in the transparent
 *            build this is the one counter that says "the radio refused the
 *            frame", and without it a bridge that drops on the radio looks
 *            identical to one that never received the frame)
 *   rxdrop   usb_tx_drop frames that could not be handed to USB
 *   refl     refl       the host's own frames, bounced back by the access point
 *   poolfail poolfail   lwIP's pbuf pool empty at the moment a frame arrived
 *   ringpk   ringpk     peak depth of the USB queue since the last line
 *
 * ringpk is the one field this architecture cannot measure as written: the
 * specification counts a Wi-Fi->host transmit queue of 32, and while the bridge
 * terminates IP at L3 there is no such queue - frames go straight to TinyUSB.
 * The queue that does exist is the USB->lwIP ring, and that is what is reported
 * here rather than a number that would be invented.
 */
size_t bridge_format_stats(char *buf, size_t len)
{
    bridge_stats_t *s = &bridge_stats;
    int n;

    n = snprintf(buf, len,
                 "stats: ->wifi=%lu ->host=%lu txdrop=%lu wdrop=%lu rxdrop=%lu refl=%lu "
                 "poolfail=%lu ringpk=%u link=%s hangs=%lu faults=%lu "
                 "faultpc=0x%08x freeram=%lu host=%s",
                 (unsigned long)s->to_wifi, (unsigned long)s->usb_tx,
                 (unsigned long)s->no_route, (unsigned long)s->wifi_drop,
                 (unsigned long)s->usb_tx_drop,
                 (unsigned long)s->refl, (unsigned long)s->poolfail,
                 (unsigned)s->ringpk,
                 bridge_link_word(),
                 (unsigned long)diagnostics_hangs(),
                 (unsigned long)diagnostics_faults(),
                 (unsigned)diagnostics_fault_pc(),
                 (unsigned long)diagnostics_free_ram(),
                 bridge_host_ipv4_word());

    s->ringpk = 0u;                 /* peak since the last line, then reset */

    /* Same clamp as the console formatters: snprintf reports the length it would
     * have written, and callers print the returned value as a byte count. */
    if (n > (int)len - 1) {
        n = (int)len - 1;
    }

    return (n > 0) ? (size_t)n : 0u;
}

/* Write the stats: line to the debug stream.  Mute until `set debug on`: this
 * is the only place the periodic diagnostics go, and it costs nothing when the
 * stream is off. */
static void bridge_print_debug_stats(void)
{
    static char line[256];

    if (!console_debug_enabled()) {
        return;
    }
    bridge_format_stats(line, sizeof(line));
    console_debug_line(line);
}



/* ---------------------------- USB -> lwIP drain -------------------------- */
static void bridge_drain_usb_rx(void)
{
    while (rx_read != rx_write) {
        frame_slot_t *slot = &rx_ring[rx_read];
        rx_read = rx_next(rx_read);

        struct pbuf *p = pbuf_alloc(PBUF_RAW, slot->len, PBUF_POOL);
        if (p == NULL) {
            /* poolfail, not just a drop: the frame was there and lwIP had
             * nowhere to put it. */
            bridge_stats.poolfail++;
            bridge_stats.usb_rx_drop++;
            continue;
        }
        pbuf_take(p, slot->data, slot->len);

#if TRACE_FRAMES
        /* The host side of the bridge, traced before lwIP gets it: this is the
         * line that shows whether a DHCP DISCOVER ever arrives. */
        bridge_trace_hex("->wifi", slot->data, slot->len);
#endif

        /* ethernet_input() takes ownership on success; free only on failure. */
        if (bridge_usb_netif.input(p, &bridge_usb_netif) != ERR_OK) {
            pbuf_free(p);
        }
    }
}

/* --------------------------------- bring up ------------------------------ */
/* The MAC TinyUSB describes to the host as iMACAddress.
 *
 * BRIDGE_L2_BRIDGE 1: adopt the station's own address.  That is what "one MAC,
 * end-to-end identity" means in practice - the host's USB interface and the
 * CYW43 station become the same Ethernet address, so the access point sees one
 * station, answers to one address, and the host is on your network rather than
 * behind the Pico.  cyw43_netif_init() has already put the station's address in
 * w0->hwaddr (cyw43_lwip.c:149, via cyw43_wifi_get_mac), which is why
 * bridge_init() initialises cyw43 before tusb_init() in this mode.
 *
 * Otherwise (and as the fallback when there is no radio): a locally administered
 * address derived from the board's unique id, so two Picos on the same desk do
 * not present the same interface to a host. */
static void bridge_init_mac(void)
{
#if BRIDGE_L2_BRIDGE
    struct netif *wn = bridge_wifi_netif();

    if ((wn != NULL) && (wn->hwaddr_len == ETH_HWADDR_LEN)) {
        memcpy(tud_network_mac_address, wn->hwaddr, ETH_HWADDR_LEN);
        printf("[bridge] usb mac adopted from the station %02x:%02x:%02x:%02x:%02x:%02x\n",
               tud_network_mac_address[0], tud_network_mac_address[1], tud_network_mac_address[2],
               tud_network_mac_address[3], tud_network_mac_address[4], tud_network_mac_address[5]);
        return;
    }
    printf("[bridge] no station MAC to adopt yet: falling back to the board id\n");
#endif

    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);

    tud_network_mac_address[0] = 0x02;   /* bit 1 set: locally administered */
    tud_network_mac_address[1] = id.id[0];
    tud_network_mac_address[2] = 'P';
    tud_network_mac_address[3] = id.id[1];
    tud_network_mac_address[4] = id.id[2];
    tud_network_mac_address[5] = id.id[3];
}

/* cyw43 + lwIP's Wi-Fi netif.  Split out because the two data paths want it at
 * different moments: the L3 path can bring the USB side up first (a host should
 * get a link and a lease even with no uplink), but the L2 path must know the
 * station's MAC before TinyUSB describes the interface. */
static void bridge_init_wifi(void)
{
#if BRIDGE_WIFI_ENABLE
    int rc = cyw43_arch_init_with_country(console_country_code());
    if (rc != 0) {
        printf("[bridge] cyw43 init failed (%d): no Wi-Fi uplink\n", rc);
        return;
    }
    wifi_present = true;
    cyw43_arch_enable_sta_mode();
#else
    printf("[bridge] Wi-Fi side disabled (BRIDGE_WIFI_ENABLE=0): USB-only build\n");
#endif
}

#if BRIDGE_L2_BRIDGE
/* Hand both ports of the bridge to the forwarder.
 *
 * cyw43 sets w0->input to ethernet_input() and starts its own DHCP client on
 * w0.  Neither survives contact with a transparent bridge: the frames must reach
 * the host rather than lwIP, and a station that leases an address for itself
 * while the host behind it leases another one is two addresses on one MAC -
 * precisely what the requirement says there must not be.  So the client is
 * stopped and the input functions are replaced. */
static void bridge_attach_l2_ports(void)
{
    struct netif *wn = bridge_wifi_netif();

    if (wn == NULL) {
        return;
    }

    /* Idempotent, because bridge_service_wifi() calls it every poll: cyw43 puts
     * its own input function back when it re-adds the interface, and printing
     * the adoption line on each of those would flood the console. */
    if ((bridge_usb_netif.input == bridge_l2_usb_input) &&
        (wn->input == bridge_l2_wifi_input)) {
        return;
    }

#if LWIP_DHCP
    dhcp_stop(wn);
#endif
    wn->input = bridge_l2_wifi_input;
    bridge_usb_netif.input = bridge_l2_usb_input;

    printf("[bridge] l2 bridge: 2 ports, host adopts %02x:%02x:%02x:%02x:%02x:%02x\n",
           tud_network_mac_address[0], tud_network_mac_address[1], tud_network_mac_address[2],
           tud_network_mac_address[3], tud_network_mac_address[4], tud_network_mac_address[5]);
}
#endif


/* The USB netif, identical in both modes except for the address it is given:
 * a router hands the host an address of its own, a bridge hands it nothing. */
static bool bridge_add_usb_netif(void)
{
    ip4_addr_t ip, mask, gw;

#if BRIDGE_L2_BRIDGE
    IP4_ADDR(&ip, 0, 0, 0, 0);
    IP4_ADDR(&mask, 0, 0, 0, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);
#else
    BRIDGE_IP4_ADDR(&ip,   BRIDGE_HOST_IP);
    BRIDGE_IP4_ADDR(&mask, BRIDGE_HOST_NETMASK);
    BRIDGE_IP4_ADDR(&gw,   BRIDGE_HOST_IP);
#endif

    if (netif_add(&bridge_usb_netif, &ip, &mask, &gw, NULL,
                  usb_netif_init, ethernet_input) == NULL) {
        printf("[bridge] FATAL: could not add the USB netif\n");
        return false;
    }
    bridge_usb_netif.linkoutput = usb_netif_linkoutput;
    bridge_usb_netif.output     = usb_netif_output;
    netif_set_link_callback(&bridge_usb_netif, usb_netif_link_cb);
    netif_set_up(&bridge_usb_netif);
    if (usb_link_up) {
        netif_set_link_up(&bridge_usb_netif);
    }

#if BRIDGE_L2_BRIDGE
    printf("[bridge] usb netif bridged, no address (the host takes one from your router)\n");
#else
    printf("[bridge] usb netif %s/%s gw %s\n",
           ip4addr_ntoa(netif_ip4_addr(&bridge_usb_netif)),
           ip4addr_ntoa(netif_ip4_netmask(&bridge_usb_netif)),
           ip4addr_ntoa(netif_ip4_gw(&bridge_usb_netif)));
#endif
    return true;
}

void bridge_init(void)
{
    stdio_uart_init();
    printf("\n[bridge] pico2-wifi-card up\n");

    /* First, before anything that can block: this is what says whether the
     * board is booting cold or after the watchdog pulled it out of a hang, and
     * it arms the watchdog that makes the next hang recoverable. */
    diagnostics_init();

    /* Settings first: the Wi-Fi side is started from the profile this loads. */
    console_init();

#if BRIDGE_L2_BRIDGE
    /* The station comes first here.  TinyUSB fixes the interface's description
     * when tusb_init() runs, and in this mode that description has to carry the
     * station's MAC, so cyw43 has to be up before it. */
    bridge_init_wifi();
    diagnostics_kick();                 /* cyw43 firmware load is the slow step */

    bridge_init_mac();

    tusb_init();
    usb_link_up = tud_mounted();
    diagnostics_kick();

    if (!bridge_add_usb_netif()) {
        return;
    }
    bridge_attach_l2_ports();
#else
    bridge_init_mac();
    printf("[bridge] usb mac %02x:%02x:%02x:%02x:%02x:%02x\n",
           tud_network_mac_address[0], tud_network_mac_address[1], tud_network_mac_address[2],
           tud_network_mac_address[3], tud_network_mac_address[4], tud_network_mac_address[5]);

    /* The USB side comes up first: a host should get a link and a lease even
     * if there is no Wi-Fi to bridge to yet. */
    tusb_init();
    usb_link_up = tud_mounted();
    diagnostics_kick();                 /* init steps are slow; keep the watchdog fed */

    if (!bridge_add_usb_netif()) {
        return;
    }

    /* Wi-Fi uplink.  cyw43 adds "w0", makes it netif_default and starts its own
     * DHCP client; we only have to ask it to associate.  The country comes from
     * the saved settings so `set country` survives a reboot. */
    bridge_init_wifi();
    if (wifi_present) {
        /* Route everything that leaves the Wi-Fi side through the NAT. */
#if BRIDGE_NAT_ENABLE
        bridge_wifi_netif()->output = bridge_nat_output;
#endif
#if BRIDGE_WIFI_ENABLE
        /* Ask for the first association attempt on the next poll rather than
         * here, so a slow cyw43 firmware load is not inside the connect path. */
        wifi_retry_at = get_absolute_time();
#endif
    }
    diagnostics_kick();                 /* cyw43 firmware load is the slow step */

    bridge_init_dhcp();
    if (dhserv_init(&dhcp_config) != ERR_OK) {
        printf("[bridge] could not start the DHCP server\n");
    } else {
        printf("[bridge] dhcp pool %d.%d.%d.%d-%d lease %ds\n",
               BRIDGE_HOST_NET_PREFIX, BRIDGE_HOST_POOL_FIRST,
               BRIDGE_HOST_POOL_FIRST + BRIDGE_HOST_POOL_SIZE - 1,
               BRIDGE_HOST_LEASE_SECONDS);
    }
#endif /* BRIDGE_L2_BRIDGE */

    bridge_initialised = true;
}

/* ---------------------------------- poll --------------------------------- */
void bridge_poll(void)
{
    /* First thing, before anything below can decide to do nothing: a board
     * whose loop is running must never be reset by its own watchdog. */
    diagnostics_kick();

    if (!bridge_initialised) {
        return;
    }

    tud_task();                 /* USB events; may queue frames into rx_ring */
    bridge_drain_usb_rx();      /* hand them to lwIP                         */
    tud_network_recv_renew();   /* ask TinyUSB for more                      */
    console_poll();             /* management console + debug stream         */
    diagnostics_report_recovery();

#if BRIDGE_WIFI_ENABLE
    cyw43_arch_poll();          /* Wi-Fi RX, and lwIP's sys_check_timeouts() */
    bridge_service_wifi();
#else
    /* cyw43_arch_poll() is what normally runs lwIP's timers; without the Wi-Fi
     * side there is no cyw43, so drive the timers here or ARP/DNS/lease
     * timeouts never fire. */
    sys_check_timeouts();
#endif
#if !BRIDGE_L2_BRIDGE
    bridge_nat_expire();
#endif

    static absolute_time_t led_at;
    static absolute_time_t status_at;
    absolute_time_t now = get_absolute_time();

    if (absolute_time_diff_us(now, led_at) <= 0) {
        led_at = make_timeout_time_ms(BRIDGE_LED_TICK_MS);
        bridge_led_service();
    }

    if (absolute_time_diff_us(now, status_at) <= 0) {
        status_at = make_timeout_time_ms(BRIDGE_STATUS_INTERVAL_MS);

        /* Keep the crash record fresh: a watchdog reset gives the firmware no
         * chance to write one, so the numbers it reports are these. */
        diagnostics_snapshot();

#if BRIDGE_VERBOSE
        bridge_print_status();          /* UART0 mirror of the same state */
#endif
        bridge_print_debug_stats();     /* the stats: line on ttyACM1     */
    }
}

