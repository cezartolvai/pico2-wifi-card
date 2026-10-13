/* ---------------------------------------------------------------------------
 * diagnostics.h - watchdog, hard fault, and the crash record that survives
 * the reset they cause.
 *
 * Implements the automatic-recovery part of the console specification
 * (openrouter-console-design.md.bak):
 * a hardware watchdog resets a firmware that stopped serving the main loop, a
 * hard-fault handler records the faulting address, and the bridge counters from
 * just before the crash are kept in RAM that crt0 does not zero, so the board
 * can print RECOVERED from ... with the counters it had when it died.
 * ------------------------------------------------------------------------ */
#ifndef PICO2_WIFI_CARD_DIAGNOSTICS_H
#define PICO2_WIFI_CARD_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>

/* Why the last reset happened. */
#define BRIDGE_REASON_NONE      0u      /* a normal power-on            */
#define BRIDGE_REASON_HANG      1u      /* watchdog timeout: loop stuck */
#define BRIDGE_REASON_FAULT     2u      /* hard fault, recorded here    */
#define BRIDGE_REASON_MANUAL    3u      /* `reboot` asked for it        */
#define BRIDGE_REASON_ASSERT    4u      /* lwIP assert, text recorded   */

/* Work out which reset brought us here, then arm the watchdog.  Call first
 * thing in bridge_init(): it must run before anything can block. */
void diagnostics_init(void);

/* Reboot from the console.  The reason is recorded first, so the next boot does
 * not report a manual reboot as a hang.  With `bootsel` the board re-enters the
 * bootrom's USB boot mode, which brings back the BOOTSEL drive without anyone
 * pressing the button. */
void diagnostics_request_reboot(bool bootsel);


/* Reload the watchdog.  Call from the poll loop, more often than the timeout. */
void diagnostics_kick(void);

/* Copy the bridge counters into the crash record.  Called from the poll loop,
 * because a watchdog reset gives the firmware no chance to record anything. */
void diagnostics_snapshot(void);

/* Print the RECOVERED line once per boot, on UART0 and on the consoles. */
void diagnostics_report_recovery(void);

/* Reached from lwIP's asserts (LWIP_PLATFORM_ASSERT, see src/lwipopts.h).  lwIP
 * names the assert that failed; recording it is the only way that name survives
 * a board whose stdio is a UART nobody is attached to.  It reboots and does not
 * return. */
void diagnostics_lwip_assert(const char *msg);

bool diagnostics_recovered(void);           /* this boot followed a hang/fault */
uint32_t diagnostics_reason(void);
uint32_t diagnostics_hangs(void);
uint32_t diagnostics_faults(void);
uint32_t diagnostics_fault_pc(void);
uint32_t diagnostics_free_ram(void);

#endif /* PICO2_WIFI_CARD_DIAGNOSTICS_H */
