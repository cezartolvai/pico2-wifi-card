/* ---------------------------------------------------------------------------
 * lwipopts.h - lwIP tuned for a bare-metal (NO_SYS=1) IPv4 router.
 *
 * Two interfaces live in this one stack:
 *   w0 - CYW43 Wi-Fi station
 *   u1 - TinyUSB CDC-NCM (the interface the USB host sees)
 * ------------------------------------------------------------------------ */
#ifndef PICO2_WIFI_CARD_LWIPOPTS_H
#define PICO2_WIFI_CARD_LWIPOPTS_H

/* No OS: everything runs from the main loop, driven by cyw43_arch_poll(). */
#define NO_SYS                          1
#define LWIP_TIMERS                     1
#define SYS_LIGHTWEIGHT_PROT            1

/* IPv4 only - the bridge (DHCP + NAT) is IPv4. */
#define LWIP_IPV6                       0
#define LWIP_SINGLE_NETIF               0
#define NETIF_MAX_NETIF                 3

/* src/config.h carries BRIDGE_L2_BRIDGE, which decides whether lwIP routes at
 * all.  It is a plain macro file with no includes of its own, so pulling it in
 * here is safe and keeps the two halves of the switch in one place. */
#include "config.h"

#if !BRIDGE_L2_BRIDGE
/* Routing: forwarding between the two interfaces, decided by the source
 * address (see bridge_hooks.h / bridge.c). */
#define IP_FORWARD                      1
#define IP_REASSEMBLY                   1
#define IP_FRAG                         1
#define IP_OPTIONS_ALLOWED              0
#define IP_TTL                          64
#define LWIP_HOOK_FILENAME              "bridge_hooks.h"
#endif

/*
 * lwIP's own asserts go through LWIP_PLATFORM_ASSERT.  pico's default is
 * panic(x): it prints to stdio - which here is UART0 only, with nobody
 * attached - and then calls exit(), whose while(1) breakpoint faults.  What
 * reached the console was "hard fault at _exit": the assert text was lost.
 * Routing asserts here keeps the message, and it survives the reset in the
 * crash record, so the console can name the assert that killed the last boot.
 */
void diagnostics_lwip_assert(const char *msg);
#define LWIP_PLATFORM_ASSERT(x)         diagnostics_lwip_assert(x)

/*
 * lwIP's own headers decide how the hook points are wired up before they ever
 * see LWIP_HOOK_FILENAME - ip4.h, for instance, turns ip4_route_src() into a
 * real function only if LWIP_HOOK_IP4_ROUTE_SRC is already defined at that
 * point.  So the hook macros have to be visible from lwipopts.h onwards;
 * bridge_hooks.h supplies the prototypes.
 *
 * In the transparent L2 build these two hooks do not exist: frames are forwarded
 * at the Ethernet level and never reach ip4_input()/ip4_route_src().  Declaring
 * the hooks anyway would make lwIP call symbols that src/bridge.c deliberately
 * did not compile, and the link fails with "undefined reference to
 * bridge_hook_route_src".  So the hook macros are defined only for the L3 build.
 */
#if !BRIDGE_L2_BRIDGE
#define LWIP_HOOK_IP4_INPUT(p, inp)         bridge_hook_ip4_input((p), (inp))
#define LWIP_HOOK_IP4_ROUTE_SRC(src, dest)  bridge_hook_route_src((src), (dest))
#endif

/* Interfaces */
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_HWADDRHINT           0
#define LWIP_NETIF_TX_SINGLE_PBUF       0

/* Protocols */
#define LWIP_ETHERNET                   1
#define LWIP_ARP                        1
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DHCP                       1     /* client, on the Wi-Fi netif */
#define LWIP_DNS                        1     /* learn the upstream resolver  */
#define LWIP_IGMP                       0
#define LWIP_NETCONN                      0
#define LWIP_SOCKET                       0
#define LWIP_HAVE_LOOPIF                0
#define LWIP_STATS                      0

/* Memory - sized for a small router rather than a web server. */
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (16 * 1024)
#define MEMP_NUM_PBUF                   16
#define PBUF_POOL_SIZE                  24
#define MEMP_NUM_UDP_PCB                6
#define MEMP_NUM_TCP_PCB                8
#define MEMP_NUM_TCP_PCB_LISTEN         4
#define MEMP_NUM_TCP_SEG                32
#define MEMP_NUM_ARP_QUEUE              10
#define MEMP_NUM_SYS_TIMEOUT            12

#define TCP_MSS                         1460
#define TCP_SND_BUF                     (4 * TCP_MSS)
#define TCP_WND                         (4 * TCP_MSS)
#define TCP_SND_QUEUELEN                16

#endif /* PICO2_WIFI_CARD_LWIPOPTS_H */
