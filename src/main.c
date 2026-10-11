/* ---------------------------------------------------------------------------
 * main.c - bring the bridge up and keep it running.
 *
 * Everything is driven from this loop: TinyUSB (NO_OS), cyw43/lwIP in poll mode
 * (NO_SYS=1) and the NAT table.  Nothing runs in an interrupt.
 * ------------------------------------------------------------------------ */
#include <stdio.h>

#include "pico/stdlib.h"

#include "bridge.h"
#include "config.h"

int main(void)
{
    bridge_init();

    for (;;) {
        bridge_poll();
    }

    return 0;
}
