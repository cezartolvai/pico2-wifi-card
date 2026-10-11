/* ---------------------------------------------------------------------------
 * diagnostics.c - watchdog, hard fault, and the record that survives them.
 *
 * Three pieces that only make sense together:
 *   - a hardware watchdog resets a firmware that stopped serving the poll loop,
 *     so the board re-enumerates instead of having to be unplugged;
 *   - a hard-fault handler records the faulting address before rebooting;
 *   - the bridge counters from just before the crash live in RAM that crt0 does
 *     not zero, so the board can say RECOVERED from ... with the numbers it had
 *     when it died.
 *
 * The record lives in .uninitialized_data (see pico/platform/sections.h): crt0
 * copies .data and zeroes .bss but leaves this section alone, so a warm reset
 * keeps it.  A cold power-on does not, which is why the record carries a magic
 * word instead of being trusted blindly.
 * ------------------------------------------------------------------------ */
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"

#include "bridge.h"
#include "config.h"
#include "console.h"
#include "diagnostics.h"

/* Cortex-M33 fault status registers, read where they live: the SDK does not
 * wrap the system control block, and pulling in CMSIS for four registers is
 * not worth the dependency. */
#define SCB_CFSR  (*(volatile uint32_t *)0xE000ED28u)
#define SCB_HFSR  (*(volatile uint32_t *)0xE000ED2Cu)
#define SCB_MMFAR (*(volatile uint32_t *)0xE000ED34u)
#define SCB_BFAR  (*(volatile uint32_t *)0xE000ED38u)

#define CRASH_MAGIC   0x43525331u       /* "CRS1": the record is usable       */
#define FRAME_WORDS   8u                /* r0 r1 r2 r3 r12 lr pc psr          */

typedef struct {
    uint32_t magic;
    uint32_t reason;                    /* BRIDGE_REASON_*                    */
    uint32_t hangs;
    uint32_t faults;
    uint32_t faultpc;
    uint32_t hfsr;
    uint32_t cfsr;
    uint32_t mmfar;
    uint32_t bfar;
    uint32_t frame[FRAME_WORDS];
    uint32_t to_wifi;                   /* bridge counters at the moment of   */
    uint32_t to_host;                   /* the crash, or of the last snapshot */
    uint32_t txdrop;
    uint32_t rxdrop;
    char assert_msg[40];                /* BRIDGE_REASON_ASSERT: the LWIP_ASSERT */
} crash_record_t;

static crash_record_t crash_record __attribute__((section(".uninitialized_data")));

static uint32_t hangs_count;
static uint32_t faults_count;
static uint32_t fault_pc;
static uint32_t reset_reason = BRIDGE_REASON_NONE;
static bool recovery_pending;
static char recovery_line[192];
static absolute_time_t recovery_deadline;


/* ------------------------- crash record bookkeeping ----------------------- */
static bool record_valid(void)
{
    return crash_record.magic == CRASH_MAGIC;
}

/* Copy the bridge counters into the record.  This runs from the poll loop, on
 * purpose: a watchdog timeout gives the firmware no chance to record anything,
 * so the freshest numbers available are the ones from the last snapshot. */
void diagnostics_snapshot(void)
{
    crash_record.magic   = CRASH_MAGIC;
    /* NONE, not the reason this boot started with: the record describes what the
     * firmware was doing when it stopped, and a board that is running normally
     * is doing nothing special.  Leaving a stale reason here would make a later
     * hang look like the reboot that came before it. */
    crash_record.reason  = BRIDGE_REASON_NONE;
    crash_record.hangs   = hangs_count;
    crash_record.faults  = faults_count;
    crash_record.faultpc = fault_pc;
    crash_record.to_wifi = bridge_stats.to_wifi;
    crash_record.to_host = bridge_stats.usb_tx;
    crash_record.txdrop  = bridge_stats.no_route;
    crash_record.rxdrop  = bridge_stats.usb_tx_drop;
}

/* ------------------------------- hard fault -------------------------------- */
/* Cortex-M pushes r0..r3, r12, lr, pc and psr onto the stack the faulting code
 * was using; which stack that was is bit 2 of EXC_RETURN in lr, so the
 * trampoline picks msp or psp and hands the frame to C. */
void diagnostics_fault(uint32_t *frame);

__attribute__((naked)) void isr_hardfault(void)
{
    __asm volatile(
        ".syntax unified              \n"
        "tst   lr, #4                 \n"
        "ite   eq                     \n"
        "mrseq r0, msp                \n"
        "mrsne r0, psp                \n"
        "b     diagnostics_fault      \n"
    );
}

void diagnostics_fault(uint32_t *frame)
{
    crash_record.magic  = CRASH_MAGIC;      /* claim the record first         */
    crash_record.reason = BRIDGE_REASON_FAULT;
    crash_record.hfsr   = SCB_HFSR;
    crash_record.cfsr   = SCB_CFSR;
    crash_record.mmfar  = SCB_MMFAR;
    crash_record.bfar   = SCB_BFAR;

    if ((frame != NULL) && (((uintptr_t)frame & 3u) == 0u)) {
        for (unsigned i = 0u; i < FRAME_WORDS; i++) {
            crash_record.frame[i] = frame[i];
        }
        crash_record.faultpc = frame[6];    /* the stacked pc: the fault site */
    } else {
        crash_record.faultpc = 0u;          /* frame unusable: nothing to log */
    }

    crash_record.to_wifi = bridge_stats.to_wifi;
    crash_record.to_host = bridge_stats.usb_tx;
    crash_record.txdrop  = bridge_stats.no_route;
    crash_record.rxdrop  = bridge_stats.usb_tx_drop;

    crash_record.hangs  = hangs_count;
    crash_record.faults = faults_count + 1u;    /* this fault is counted now  */

    /* Warm reboot into the normal boot path.  Nothing here may block: XIP is
     * already suspect, so no printf and no flash write. */
    watchdog_reboot(0u, 0u, 100u);
    for (;;) {
    }
}



/* ------------------------------- lwIP assert -------------------------------- */
/* Reached from LWIP_PLATFORM_ASSERT (see src/lwipopts.h).  Unlike a CPU fault
 * this one is not mysterious: lwIP names the assert that failed, and that text
 * is the whole diagnostic.  Keep it, count it as a fault, reboot warm.
 *
 * Nothing here may block or print: the assert can fire from inside lwIP while a
 * pbuf is in hand, and stdio here is UART0 with nobody attached to read it. */
void diagnostics_lwip_assert(const char *msg)
{
    crash_record.magic  = CRASH_MAGIC;
    crash_record.reason = BRIDGE_REASON_ASSERT;

    if (msg != NULL) {
        size_t n = 0u;
        while ((n + 1u < sizeof(crash_record.assert_msg)) && msg[n] != '\0') {
            crash_record.assert_msg[n] = msg[n];
            n++;
        }
        crash_record.assert_msg[n] = '\0';
    } else {
        crash_record.assert_msg[0] = '\0';
    }

    crash_record.to_wifi = bridge_stats.to_wifi;
    crash_record.to_host = bridge_stats.usb_tx;
    crash_record.txdrop  = bridge_stats.no_route;
    crash_record.rxdrop  = bridge_stats.usb_tx_drop;

    /* An assert has no stacked frame to read, so there is no stacked pc.  The
     * address that called lwIP's assert hook is the next best thing, and it is
     * worth recording: `faults` is incremented here, and a count that moves while
     * `faultpc` stays 0x00000000 is exactly the unreadable case - the board says
     * it recovered from something and gives nothing to map. */
    crash_record.faultpc = (uint32_t)(uintptr_t)__builtin_return_address(0u);

    crash_record.hangs  = hangs_count;
    crash_record.faults = faults_count + 1u;    /* this assert is counted now */

    watchdog_reboot(0u, 0u, 100u);
    for (;;) {
    }
}


/* ------------------------------- which reset? ----------------------------- */
void diagnostics_init(void)
{
    /* The watchdog scratch registers are the one thing the SDK guarantees
     * survive its own reset, so they decide whether this boot is warm. */
    bool warm = watchdog_caused_reboot() && record_valid();

    if (warm) {
        hangs_count  = crash_record.hangs;
        faults_count = crash_record.faults;
        fault_pc     = crash_record.faultpc;

        if (crash_record.reason == BRIDGE_REASON_FAULT) {
            /* The fault handler counted it before rebooting: report it, do not
             * count the same fault twice. */
            reset_reason = BRIDGE_REASON_FAULT;
        } else if (crash_record.reason == BRIDGE_REASON_ASSERT) {
            /* Same for an lwIP assert: it counted itself before rebooting. */
            reset_reason = BRIDGE_REASON_ASSERT;
        } else if (crash_record.reason == BRIDGE_REASON_MANUAL) {
            /* `reboot` asked for this reset.  It is neither a hang nor a fault,
             * and it is not a recovery either: counting it as one would make
             * `hangs` lie. */
            reset_reason = BRIDGE_REASON_MANUAL;
        } else {
            /* Warm reset with no fault recorded: the loop stopped serving the
             * watchdog, and the watchdog did the recovering. */
            hangs_count++;
            reset_reason = BRIDGE_REASON_HANG;
        }
    } else {
        /* Cold power-on (or RAM did not survive): the totals start over, which
         * is what "since the last cold power-on" means. */
        memset(&crash_record, 0, sizeof(crash_record));
        hangs_count  = 0u;
        faults_count = 0u;
        fault_pc     = 0u;
        reset_reason = BRIDGE_REASON_NONE;
    }

    crash_record.magic   = CRASH_MAGIC;
    crash_record.reason  = reset_reason;
    crash_record.hangs   = hangs_count;
    crash_record.faults  = faults_count;
    crash_record.faultpc = fault_pc;

    recovery_pending = (reset_reason == BRIDGE_REASON_HANG) ||
                       (reset_reason == BRIDGE_REASON_FAULT) ||
                       (reset_reason == BRIDGE_REASON_ASSERT);

    /* Arm the watchdog last: from here on the poll loop has to keep kicking it. */
    watchdog_enable(BRIDGE_WATCHDOG_MS, true);
    watchdog_update();
}

void diagnostics_kick(void)
{
    watchdog_update();
}

/* Reboot on request from the console.  The reason is written into the crash
 * record first: without it the next boot would see a watchdog reset and report
 * a manual reboot as a hang. */
void diagnostics_request_reboot(bool bootsel)
{
    crash_record.magic   = CRASH_MAGIC;
    crash_record.reason  = BRIDGE_REASON_MANUAL;
    crash_record.hangs   = hangs_count;
    crash_record.faults  = faults_count;
    crash_record.faultpc = fault_pc;

    if (bootsel) {
        /* Back into the bootrom's USB boot mode: the BOOTSEL drive reappears
         * with nobody having to press the button. */
        reset_usb_boot(0u, 0u);
    }
    watchdog_reboot(0u, 0u, 0u);
    for (;;) {
    }
}


/* ------------------------------- reporting -------------------------------- */
bool diagnostics_recovered(void)
{
    return (reset_reason == BRIDGE_REASON_HANG) ||
           (reset_reason == BRIDGE_REASON_FAULT) ||
           (reset_reason == BRIDGE_REASON_ASSERT);
}


uint32_t diagnostics_reason(void)
{
    return reset_reason;
}

uint32_t diagnostics_hangs(void)
{
    return hangs_count;
}

uint32_t diagnostics_faults(void)
{
    return faults_count;
}

uint32_t diagnostics_fault_pc(void)
{
    return fault_pc;
}

/* Free RAM is what is left above the static image: the heap region, which is
 * where any new buffer would have to come from.  __end__ is the end of .bss
 * (where the heap starts) and __StackLimit is the top of RAM. */
extern char __end__[];
extern char __StackLimit[];

uint32_t diagnostics_free_ram(void)
{
    uintptr_t first = (uintptr_t)&__end__;
    uintptr_t last = (uintptr_t)&__StackLimit;

    return (last > first) ? (uint32_t)(last - first) : 0u;
}

/* HFSR carries four bits of meaning and nothing else.  A bare hex word in a log
 * line is not readable, and reading it wrong is easy: 0x40000000 is LOCKUP, not
 * FORCED - FORCED is bit 0.  Name the bit that is actually set. */
static const char *hfsr_word(uint32_t hfsr)
{
    if (hfsr & 0x80000000u) {
        return "DEBUGEVENT";   /* bit 31: the core entered debug state   */
    }
    if (hfsr & 0x40000000u) {
        return "LOCKUP";       /* bit 30: the fault handler itself faulted     */
    }
    if (hfsr & 0x00000002u) {
        return "VECTASSERT";   /* bit 1: a vector table read failed            */
    }
    if (hfsr & 0x00000001u) {
        return "FORCED";       /* bit 0: an escalatable fault reached here     */
    }
    return "NONE";
}

void diagnostics_report_recovery(void)
{
    if (!recovery_pending || (reset_reason == BRIDGE_REASON_NONE)) {
        return;
    }

    if (recovery_line[0] == '\0') {
        if (reset_reason == BRIDGE_REASON_ASSERT) {
            /* lwIP named the assert that failed; that name is the report. */
            snprintf(recovery_line, sizeof(recovery_line),
                     "RECOVERED from an lwIP assert: %s at 0x%08x "
                     "(->wifi=%lu ->host=%lu txdrop=%lu rxdrop=%lu)",
                     (crash_record.assert_msg[0] != '\0') ? crash_record.assert_msg
                                                          : "(no message recorded)",
                     (unsigned)crash_record.faultpc,
                     (unsigned long)crash_record.to_wifi, (unsigned long)crash_record.to_host,
                     (unsigned long)crash_record.txdrop, (unsigned long)crash_record.rxdrop);
        } else if (reset_reason == BRIDGE_REASON_FAULT) {
            snprintf(recovery_line, sizeof(recovery_line),
                     "RECOVERED from hard fault at 0x%08x (hfsr=0x%08x %s cfsr=0x%08x "
                     "mmfar=0x%08x bfar=0x%08x)",
                     (unsigned)crash_record.faultpc, (unsigned)crash_record.hfsr,
                     hfsr_word(crash_record.hfsr), (unsigned)crash_record.cfsr,
                     (unsigned)crash_record.mmfar, (unsigned)crash_record.bfar);
        } else {

            snprintf(recovery_line, sizeof(recovery_line),
                     "RECOVERED from a hang: the watchdog reset the firmware "
                     "(->wifi=%lu ->host=%lu txdrop=%lu rxdrop=%lu)",
                     (unsigned long)crash_record.to_wifi, (unsigned long)crash_record.to_host,
                     (unsigned long)crash_record.txdrop, (unsigned long)crash_record.rxdrop);
        }
        /* UART0 always: it is there whether the host has enumerated or not. */
        printf("[bridge] %s\n", recovery_line);
        recovery_deadline = make_timeout_time_ms(BRIDGE_RECOVERY_HOLD_MS);
    }

    /* The CDC consoles may not be configured by the host yet at boot.  Hold the
     * line until one of them is open, so the trace of a self-healing crash is
     * not lost in the first seconds after enumeration; after the hold, UART0
     * already carries it and the line is dropped rather than queued forever. */
    if (console_reachable()) {
        console_recovered_line(recovery_line);
        recovery_pending = false;
        return;
    }
    if (absolute_time_diff_us(get_absolute_time(), recovery_deadline) <= 0) {
        recovery_pending = false;
    }
}

