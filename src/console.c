/* ---------------------------------------------------------------------------
 * console.c - management console (/dev/ttyACM0), debug stream (/dev/ttyACM1)
 * and the runtime settings those two can change.
 *
 * Implements .clinerules/06-console-spec.md.  Nothing here blocks: when the
 * CDC-ACM functions are not enumerated yet every routine does nothing, so the
 * bridge keeps running and UART0 stays usable as a mirror.
 * ------------------------------------------------------------------------ */
#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>     /* strtol, for use <n> / join <n> */
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"
#include "tusb.h"

#include "cyw43.h"
#include "pico/cyw43_arch.h"
#include "hardware/flash.h"

#include "bridge.h"
#include "config.h"
#include "console.h"
#include "diagnostics.h"     /* reboot / BOOTSEL from the console */
#include "nat.h"             /* bridge_nat_active(), for the state dump */


/* TinyUSB numbers the CDC-ACM functions in descriptor order: instance 0 is the
 * management console (/dev/ttyACM0), instance 1 the debug stream (/dev/ttyACM1). */
#define ITF_MGMT            0u
#define ITF_DEBUG           1u

#define CONSOLE_VERSION     "1.1.0"
#define BRIDGE_LINE_MAX     96u
#define BRIDGE_SCAN_MAX     12u

/* ------------------------------ settings in flash ------------------------- */
/* The record lives in the LAST flash sector, away from the program image at the
 * start of the chip: reflashing a .uf2 leaves the saved profiles intact.     */
#define SETTINGS_MAGIC      0x50434631u   /* "1SCP" */
#define SETTINGS_VERSION    1u
#define SETTINGS_SECTOR     4096u
#define SETTINGS_OFFS       (PICO_FLASH_SIZE_BYTES - SETTINGS_SECTOR)
#define SETTINGS_HDR        16u

bridge_settings_t bridge_settings;

static uint8_t settings_image[SETTINGS_SECTOR];

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;

    for (size_t i = 0u; i < n; i++) {
        crc ^= (uint32_t)p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

/* flash_range_program() needs a whole number of pages. */
static size_t settings_program_bytes(void)
{
    size_t n = SETTINGS_HDR + sizeof(bridge_settings_t);

    return (n + (FLASH_PAGE_SIZE - 1u)) & ~(size_t)(FLASH_PAGE_SIZE - 1u);
}

/* A blank or half-written sector is the normal state on a board that has never
 * been provisioned, so a failed read is the default, not an error. */
static bool settings_load(bridge_settings_t *out)
{
    memcpy(settings_image, (const void *)(XIP_BASE + SETTINGS_OFFS), SETTINGS_SECTOR);

    uint32_t magic, version, crc;
    memcpy(&magic,   settings_image + 0u,  4u);
    memcpy(&version, settings_image + 4u, 4u);
    memcpy(&crc,     settings_image + 12u, 4u);

    if ((magic != SETTINGS_MAGIC) || (version != SETTINGS_VERSION)) {
        return false;
    }

    bridge_settings_t rec;
    memcpy(&rec, settings_image + SETTINGS_HDR, sizeof(rec));
    if (crc32((const uint8_t *)&rec, sizeof(rec)) != crc) {
        return false;
    }
    if (rec.count > BRIDGE_PROFILE_MAX) {
        return false;
    }

    *out = rec;
    return true;
}

static bool settings_save(const bridge_settings_t *in)
{
    memset(settings_image, 0xff, sizeof(settings_image));

    bridge_settings_t rec;
    memcpy(&rec, in, sizeof(rec));

    uint32_t magic   = SETTINGS_MAGIC;
    uint32_t version = SETTINGS_VERSION;
    uint32_t crc     = crc32((const uint8_t *)&rec, sizeof(rec));

    memcpy(settings_image + 0u,   &magic,   4u);
    memcpy(settings_image + 4u,   &version, 4u);
    memcpy(settings_image + 12u,  &crc,     4u);
    memcpy(settings_image + SETTINGS_HDR, in, sizeof(*in));

    /* Erasing and programming take XIP away for a few tens of ms.  The SDK's
     * helpers are copied to RAM and put XIP back afterwards, so this is safe
     * from the poll loop - and it is why persistence happens on `save` and
     * never in the data path. */
    flash_range_erase(SETTINGS_OFFS, SETTINGS_SECTOR);
    flash_range_program(SETTINGS_OFFS, settings_image, settings_program_bytes());
    return true;
}

/* ------------------------------ console output ---------------------------- */
static void console_write(uint8_t itf, const char *s, size_t n)
{
    if (!tud_cdc_n_ready(itf)) {
        return;                     /* no console yet: drop, never block */
    }
    tud_cdc_n_write(itf, s, n);
}

static void console_write_str(uint8_t itf, const char *s)
{
    console_write(itf, s, strlen(s));
}

static void console_printf(uint8_t itf, const char *fmt, ...)
{
    static char buf[320];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    /* vsnprintf returns the length it *would* have written.  Forwarding that
     * number after a truncation sends bytes that are past the end of buf, which
     * is what the state dump started doing once it grew past 256 octeți. */
    if (n > (int)sizeof(buf) - 1) {
        n = (int)sizeof(buf) - 1;
    }
    if (n > 0) {
        console_write(itf, buf, (size_t)n);
    }
}

static void console_flush(uint8_t itf)
{
    tud_cdc_n_write_flush(itf);
}

bool console_debug_enabled(void)
{
    return bridge_settings.debug && tud_cdc_n_ready(ITF_DEBUG);
}

void console_debug_line(const char *text)
{
    if (!console_debug_enabled()) {
        return;                     /* mute until `set debug on` */
    }
    console_write_str(ITF_DEBUG, text);
    console_write(ITF_DEBUG, "\n", 1u);
    console_flush(ITF_DEBUG);
}

void console_debug_logf(const char *fmt, ...)
{
    static char buf[320];
    va_list ap;

    if (!console_debug_enabled()) {
        return;
    }
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1) {
        n = (int)sizeof(buf) - 1;   /* same clamp as console_printf() */
    }
    if (n > 0) {
        console_write(ITF_DEBUG, buf, (size_t)n);
        console_flush(ITF_DEBUG);
    }
}

/* A crash that recovered itself is the one event the debug stream must never
 * swallow, so this bypasses `set debug` and reaches both consoles. */
void console_recovered_line(const char *text)
{
    size_t n = strlen(text);

    console_write(ITF_MGMT, text, n);
    console_write(ITF_MGMT, "\n", 1u);
    console_flush(ITF_MGMT);

    console_write(ITF_DEBUG, text, n);
    console_write(ITF_DEBUG, "\n", 1u);
    console_flush(ITF_DEBUG);
}



/* ------------------------------- settings API ---------------------------- */
const wifi_profile_t *console_active_profile(void)
{
    if ((bridge_settings.active == BRIDGE_NO_PROFILE) ||
        (bridge_settings.active >= bridge_settings.count)) {
        return NULL;                /* nothing configured: nothing to join */
    }
    return &bridge_settings.profile[bridge_settings.active];
}

uint32_t console_country_code(void)
{
    if (bridge_settings.country[0] == '\0') {
        return BRIDGE_WIFI_COUNTRY;   /* nothing chosen yet: keep the default */
    }
    return CYW43_COUNTRY(bridge_settings.country[0], bridge_settings.country[1], 0);
}

/* --------------------------------- REPL state ---------------------------- */
static char   line[BRIDGE_LINE_MAX];
static size_t line_len;
static bool   scan_menu;            /* inside the scan sub-menu             */
static bool   scan_running;         /* a cyw43 scan is in flight            */
static bool   scan_live;            /* live mode: keep rescanning           */
static bool   banner_shown;

typedef struct {
    char    ssid[BRIDGE_SSID_MAX_LEN + 1u];
    uint8_t bssid[6];
    int16_t rssi;
} scan_entry_t;

static scan_entry_t scan_list[BRIDGE_SCAN_MAX];
static uint8_t scan_count;

/* The LED's double-flash pattern asks whether live scan is running. */
bool console_scan_live(void)
{
    return scan_live;
}

/* True when the host has opened at least one of the two consoles.  Used by the
 * recovery report, which is worth holding for a few seconds rather than
 * dropping it while the host is still configuring the CDC functions. */
bool console_reachable(void)
{
    /* The CDC functions exist as soon as the device is configured; DTR is not
     * required, so `cat /dev/ttyACM0` counts as reachable too. */
    return tud_cdc_n_ready(ITF_MGMT) || tud_cdc_n_ready(ITF_DEBUG);
}



/* console_prompt() lives further down, next to the rest of the REPL. */
static void console_prompt(void);

/* --------------------------- profile bookkeeping ------------------------- */
/* `set ssid` on a device with no profiles creates the first one, exactly as the
 * specification's provisioning example does. */
static void profile_ensure(void)
{
    if (bridge_settings.count == 0u) {
        memset(bridge_settings.profile, 0, sizeof(bridge_settings.profile));
        bridge_settings.count  = 1u;
        bridge_settings.active = 0u;
    }
    if (bridge_settings.active >= bridge_settings.count) {
        bridge_settings.active = 0u;
    }
}

static void profile_drop(uint8_t idx)
{
    if (bridge_settings.count == 0u) {
        return;
    }
    for (uint8_t i = idx; (i + 1u) < bridge_settings.count; i++) {
        bridge_settings.profile[i] = bridge_settings.profile[i + 1u];
    }
    bridge_settings.count--;
    memset(&bridge_settings.profile[bridge_settings.count], 0,
           sizeof(bridge_settings.profile[0]));

    if (bridge_settings.count == 0u) {
        bridge_settings.active = BRIDGE_NO_PROFILE;
    } else if (idx < bridge_settings.active) {
        bridge_settings.active--;
    } else if (bridge_settings.active >= bridge_settings.count) {
        bridge_settings.active = 0u;
    }
}

/* --------------------------------- scanning ------------------------------ */
#if BRIDGE_WIFI_ENABLE
static int console_scan_cb(void *env, const cyw43_ev_scan_result_t *r)
{
    (void)env;

    if (r->ssid_len == 0u) {
        return 0;                   /* hidden network: nothing to show      */
    }
    for (uint8_t i = 0u; i < scan_count; i++) {
        if (memcmp(scan_list[i].bssid, r->bssid, sizeof(r->bssid)) == 0) {
            scan_list[i].rssi = r->rssi;
            return 0;               /* same AP, repeated probe frame        */
        }
    }
    if (scan_count >= BRIDGE_SCAN_MAX) {
        return 0;
    }

    scan_entry_t *e = &scan_list[scan_count++];
    size_t n = (r->ssid_len <= BRIDGE_SSID_MAX_LEN) ? r->ssid_len : BRIDGE_SSID_MAX_LEN;
    memcpy(e->ssid, r->ssid, n);
    e->ssid[n] = '\0';
    memcpy(e->bssid, r->bssid, sizeof(e->bssid));
    e->rssi = r->rssi;

    if (scan_live) {
        console_printf(ITF_MGMT, "      %2d  %-*s  %4d dBm  %02x:%02x:%02x:%02x:%02x:%02x\n",
                       (int)scan_count, BRIDGE_SSID_MAX_LEN, e->ssid, (int)r->rssi,
                       r->bssid[0], r->bssid[1], r->bssid[2],
                       r->bssid[3], r->bssid[4], r->bssid[5]);
    }
    return 0;
}
#endif

static void scan_start(void)
{
#if BRIDGE_WIFI_ENABLE
    scan_count = 0u;
    scan_menu  = true;

    cyw43_wifi_scan_options_t opts;
    memset(&opts, 0, sizeof(opts));
    if (cyw43_wifi_scan(&cyw43_state, &opts, NULL, console_scan_cb) != 0) {
        console_write_str(ITF_MGMT, "[!] scan failed: radio busy\n");
        return;
    }
    scan_running = true;
    console_write_str(ITF_MGMT, "[*] scanning...\n");
    console_flush(ITF_MGMT);
#else
    console_write_str(ITF_MGMT, "[!] no radio in this build\n");
#endif
}

static void scan_report(void)
{
    scan_running = false;

    console_printf(ITF_MGMT, "    networks (%u):\n", (unsigned)scan_count);
    for (uint8_t i = 0u; i < scan_count; i++) {
        console_printf(ITF_MGMT, "      %2d  %-*s  %4d dBm  %02x:%02x:%02x:%02x:%02x:%02x\n",
                       (int)i + 1, BRIDGE_SSID_MAX_LEN, scan_list[i].ssid,
                       (int)scan_list[i].rssi,
                       scan_list[i].bssid[0], scan_list[i].bssid[1], scan_list[i].bssid[2],
                       scan_list[i].bssid[3], scan_list[i].bssid[4], scan_list[i].bssid[5]);
    }
    if (scan_count == 0u) {
        console_write_str(ITF_MGMT, "      (none found)\n");
    }
    console_flush(ITF_MGMT);
}

/* A cyw43 scan finishes asynchronously: the results arrive through the callback
 * while cyw43_arch_poll() runs, so the report is emitted from the poll loop and
 * not from the command that started the scan. */
static void scan_service(void)
{
    if (!scan_running) {
        return;
    }
#if BRIDGE_WIFI_ENABLE
    if (cyw43_wifi_scan_active(&cyw43_state)) {
        return;
    }
    scan_report();

    if (scan_live && !bridge_wifi_ready()) {
        scan_start();               /* live mode keeps the list fresh */
    } else {
        scan_live = false;
    }
#else
    scan_running = false;           /* no radio in this build */
#endif
}

/* ------------------------------ input helpers ---------------------------- */
/* Split one word off the line and NUL-terminate it, leaving *cursor on the
 * rest.  Values (an SSID can contain spaces) are read from that rest. */
static char *next_token(char **cursor)
{
    while (isspace((unsigned char)**cursor)) {
        (*cursor)++;
    }
    if (**cursor == '\0') {
        return NULL;
    }
    char *tok = *cursor;
    while ((**cursor != '\0') && !isspace((unsigned char)**cursor)) {
        (*cursor)++;
    }
    if (**cursor != '\0') {
        **cursor = '\0';
        (*cursor)++;
    }
    return tok;
}

static void trim_trailing(char *s)
{
    size_t n = strlen(s);

    while ((n > 0u) && isspace((unsigned char)s[n - 1u])) {
        s[--n] = '\0';
    }
}

/* --------------------------------- commands ------------------------------ */
static void cmd_list(void)
{
    if (bridge_settings.count == 0u) {
        console_write_str(ITF_MGMT, "    (no profiles)\n");
        return;
    }
    for (uint8_t i = 0u; i < bridge_settings.count; i++) {
        console_printf(ITF_MGMT, "      %d%c %-*s  %s\n", i + 1,
                       (i == bridge_settings.active) ? '*' : ' ',
                       BRIDGE_SSID_MAX_LEN, bridge_settings.profile[i].ssid,
                       bridge_settings.profile[i].pass[0] ? "pass set" : "open");
    }
}

static void cmd_set(char *rest)
{
    char *key = next_token(&rest);

    if (key == NULL) {
        console_write_str(ITF_MGMT, "[!] set ssid|pass|country|debug\n");
        return;
    }

    if (strcmp(key, "ssid") == 0) {
        profile_ensure();
        char *ssid = rest;

        while (isspace((unsigned char)*ssid)) {
            ssid++;
        }
        trim_trailing(ssid);
        strncpy(bridge_settings.profile[bridge_settings.active].ssid, ssid, BRIDGE_SSID_MAX_LEN);
        bridge_settings.profile[bridge_settings.active].ssid[BRIDGE_SSID_MAX_LEN] = '\0';
        console_write_str(ITF_MGMT, "[*] applying -- re-associating\n");
        bridge_wifi_restart();
    } else if (strcmp(key, "pass") == 0) {
        if (bridge_settings.count == 0u) {
            console_write_str(ITF_MGMT, "[!] no profile: set ssid first\n");
            return;
        }
        char *pass = rest;

        while (isspace((unsigned char)*pass)) {
            pass++;
        }
        trim_trailing(pass);
        strncpy(bridge_settings.profile[bridge_settings.active].pass, pass, BRIDGE_PASS_MAX_LEN);
        bridge_settings.profile[bridge_settings.active].pass[BRIDGE_PASS_MAX_LEN] = '\0';
        console_write_str(ITF_MGMT, "[*] applying -- re-associating\n");
        bridge_wifi_restart();
    } else if (strcmp(key, "country") == 0) {
        char *cc = next_token(&rest);

        if (cc == NULL) {
            console_write_str(ITF_MGMT, "[!] set country <CC|WORLDWIDE>\n");
            return;
        }
        if ((strcasecmp(cc, "WORLDWIDE") == 0) || (cc[0] == 'X' && cc[1] == 'X')) {
            bridge_settings.country[0] = 'X';
            bridge_settings.country[1] = 'X';
        } else if (isalpha((unsigned char)cc[0]) && (cc[1] != '\0')) {
            bridge_settings.country[0] = (char)toupper((unsigned char)cc[0]);
            bridge_settings.country[1] = (char)toupper((unsigned char)cc[1]);
        } else {
            console_write_str(ITF_MGMT, "[!] two-letter country code, or WORLDWIDE\n");
            return;
        }
        /* cyw43 applies a country code when it is initialised; a runtime change
         * only takes full effect (regulatory tables included) at the next boot. */
        console_write_str(ITF_MGMT, "[*] country applied fully at the next boot\n");
    } else if (strcmp(key, "debug") == 0) {
        char *on = next_token(&rest);

        bridge_settings.debug = (on != NULL) && (strcasecmp(on, "on") == 0);
        console_write_str(ITF_MGMT, bridge_settings.debug ? "[*] debug on\n" : "[*] debug off\n");
    } else {
        console_write_str(ITF_MGMT, "[!] set ssid|pass|country|debug\n");
    }
}

/* `use <n>` and `del <n>` differ only in what they do with the index. */
static void cmd_index(const char *verb, char *rest)
{
    long n = strtol(rest, NULL, 10);

    if ((bridge_settings.count == 0u) || (n < 1) || (n > (long)bridge_settings.count)) {
        console_printf(ITF_MGMT, "[!] %s <n>\n", verb);
        return;
    }
    uint8_t idx = (uint8_t)(n - 1);

    if (verb[0] == 'u') {
        bridge_settings.active = idx;
        console_write_str(ITF_MGMT, "[*] applying -- re-associating\n");
    } else {
        profile_drop(idx);
        console_write_str(ITF_MGMT, "[*] deleted\n");
    }
    bridge_wifi_restart();
}

/* `join <n>` stages the n-th scan result as the active profile with an empty
 * password, ready for `set pass` - the flow the specification's example shows. */
static void scan_join(char *rest)
{
    long n = strtol(rest, NULL, 10);

    if ((n < 1) || ((size_t)n > scan_count)) {
        console_write_str(ITF_MGMT, "[!] join <n>\n");
        return;
    }
    profile_ensure();
    strncpy(bridge_settings.profile[bridge_settings.active].ssid,
            scan_list[n - 1].ssid, BRIDGE_SSID_MAX_LEN);
    bridge_settings.profile[bridge_settings.active].ssid[BRIDGE_SSID_MAX_LEN] = '\0';
    bridge_settings.profile[bridge_settings.active].pass[0] = '\0';

    scan_menu = false;
    scan_live = false;
    console_write_str(ITF_MGMT, "[*] staged -- set pass <password> then save\n");
}

#if BRIDGE_SELFTEST
/* Not in the specification's table and not in an image meant to be shipped:
 * BRIDGE_SELFTEST is 0 unless the build asks for it with -DBRIDGE_SELFTEST=1,
 * and such a build lives in its own directory (build-selftest).
 *
 * These verbs exist because the recovery paths of src/diagnostics.c cannot be
 * validated any other way on this board: a watchdog timeout, a CPU fault and an
 * lwIP assert each have to be caused on purpose before the board can be watched
 * recovering from them, and USB is the only channel to this board. */
static void cmd_test(char *rest)
{
    char *what = next_token(&rest);

    if (what == NULL) {
        console_write_str(ITF_MGMT, "    test hang|fault|assert|led\n");
        return;
    }

    if (strcmp(what, "led") == 0) {
        char *name = next_token(&rest);

        if (name != NULL) {
            /* Hold one pattern long enough to be watched on the board or timed
             * from the host, whatever the state would have shown instead. */
            char *sec = next_token(&rest);
            long seconds = (sec != NULL) ? strtol(sec, NULL, 10) : 10;

            if ((seconds < 1) || (seconds > 60)) {
                seconds = 10;
            }
            if (bridge_led_force(name, (uint32_t)seconds)) {
                console_printf(ITF_MGMT, "[*] led \"%s\" held for %ld s\n", name, seconds);
            } else {
                console_write_str(ITF_MGMT, "[!] led off|solid|double|5hz|1hz [seconds]\n");
            }
            return;
        }

        /* The pattern being driven, the pattern the state asked for (different
         * while `test led <name>` holds one), the level written, and where in
         * the ten-tick cycle it sits: enough to confirm a pattern and to time it
         * by sampling from the host. */
        console_printf(ITF_MGMT,
                       "    led: %-7s level %-3s tick %2lu   state asks %-7s\n"
                       "    usb %-4s wifi %-4s profiles %u live %s\n",
                       bridge_led_pattern(),
                       bridge_led_level() ? "on" : "off",
                       (unsigned long)bridge_led_tick(),
                       bridge_led_state(),
                       bridge_usb_ready() ? "up" : "down",
                       bridge_wifi_ready() ? "up" : "down",
                       (unsigned)bridge_settings.count,
                       console_scan_live() ? "yes" : "no");
        return;
    }

    if (strcmp(what, "hang") == 0) {
        console_write_str(ITF_MGMT, "[*] hanging the poll loop; watchdog fires in 4 s\n");
        console_flush(ITF_MGMT);
        /* diagnostics_kick() is the first call in bridge_poll() and this runs
         * inside bridge_poll(), so nothing reloads the watchdog from here on. */
        for (;;) {
        }
    } else if (strcmp(what, "fault") == 0) {
        console_write_str(ITF_MGMT, "[*] taking a CPU fault on purpose\n");
        console_flush(ITF_MGMT);
        /* udf -> UsageFault.  SHCSR resets to 0 and nothing in the SDK enables
         * the UsageFault handler, so the fault escalates to HardFault and lands
         * in isr_hardfault, which records the address and reboots warm. */
        __builtin_trap();
    } else if (strcmp(what, "assert") == 0) {
        console_write_str(ITF_MGMT, "[*] taking an lwIP assert\n");
        console_flush(ITF_MGMT);
        diagnostics_lwip_assert("selftest: deliberate assert");
    } else {
        console_write_str(ITF_MGMT, "[!] test hang|fault|assert|led\n");
    }
}
#endif

static void console_exec(void)
{
    char *cursor = line;
    char *verb = next_token(&cursor);

    if (verb == NULL) {
        console_dump_state();       /* an empty line just reprints the state */
        console_flush(ITF_MGMT);
        return;
    }

    /* Case-insensitive verbs, kept short: the handlers only ever compare
     * against a fixed set of words. */
    char cmd[16];
    size_t n = strlen(verb);

    if (n > sizeof(cmd) - 1u) {
        n = sizeof(cmd) - 1u;
    }
    for (size_t i = 0u; i < n; i++) {
        cmd[i] = (char)tolower((unsigned char)verb[i]);
    }
    cmd[n] = '\0';

    if (strcmp(cmd, "set") == 0) {
        cmd_set(cursor);
    } else if (strcmp(cmd, "list") == 0) {
        cmd_list();
    } else if (strcmp(cmd, "use") == 0) {
        cmd_index("use", cursor);
    } else if (strcmp(cmd, "del") == 0) {
        cmd_index("del", cursor);
    } else if (strcmp(cmd, "scan") == 0) {
        scan_start();
    } else if (strcmp(cmd, "join") == 0) {
        scan_join(cursor);
    } else if (strcmp(cmd, "live") == 0) {
        scan_live = true;
        scan_start();
    } else if (strcmp(cmd, "back") == 0) {
        scan_menu = false;
        scan_live = false;
    } else if (strcmp(cmd, "save") == 0) {
        console_write_str(ITF_MGMT, settings_save(&bridge_settings) ? "[*] saved to flash\n"
                                                                  : "[!] save failed\n");
    } else if (strcmp(cmd, "restore") == 0) {
        if (settings_load(&bridge_settings)) {
            console_write_str(ITF_MGMT, "[*] restored from flash\n");
        } else {
            console_write_str(ITF_MGMT, "[!] nothing saved in flash\n");
        }
        bridge_wifi_restart();
    } else if (strcmp(cmd, "stats") == 0) {
        /* The same line the debug stream gets, asked for on the console that is
         * actually open: bridge_print_status() would send it to UART0 instead,
         * where nobody is looking. */
        char line[256];

        bridge_format_stats(line, sizeof(line));
        console_write_str(ITF_MGMT, line);
        console_write(ITF_MGMT, "\n", 1u);
    } else if (strcmp(cmd, "reboot") == 0) {
        /* Not in the specification's table: this bridge is reflashed a lot while
         * it is being debugged, and `reboot bootsel` brings the BOOTSEL drive
         * back without anyone having to hold the button. */
        char *what = next_token(&cursor);
        bool bootsel = (what != NULL) && (strcmp(what, "bootsel") == 0);

        console_write_str(ITF_MGMT, bootsel ? "[*] entering BOOTSEL mode\n"
                                            : "[*] rebooting\n");
        console_flush(ITF_MGMT);
        diagnostics_request_reboot(bootsel);
#if BRIDGE_SELFTEST
    } else if (strcmp(cmd, "test") == 0) {
        cmd_test(cursor);
#endif
    } else if ((strcmp(cmd, "help") == 0) || (strcmp(cmd, "?") == 0)) {
        console_write_str(ITF_MGMT,
                          "    set ssid|pass|country|debug    list|use|del\n"
                          "    scan  join <n>  live  back\n"
                          "    save  restore  stats  reboot [bootsel]  help\n");
#if BRIDGE_SELFTEST
        console_write_str(ITF_MGMT, "    test hang|fault|assert|led   (self-test build)\n");
#endif
    } else {
        console_printf(ITF_MGMT, "[!] unknown command \"%s\"\n", cmd);
    }

    console_dump_state();
    console_prompt();
    console_flush(ITF_MGMT);
}

/* --------------------------------- state dump ---------------------------- */
static const char *ssid_word(void)
{
    const wifi_profile_t *p = console_active_profile();

    return (p != NULL) ? p->ssid : "(unset)";
}

static const char *pass_word(void)
{
    const wifi_profile_t *p = console_active_profile();

    if ((p == NULL) || (p->pass[0] == '\0')) {
        return "unset";
    }
    return "set";                       /* never echo the passphrase back */
}

static const char *country_word(void)
{
    static char cc[3];

    if (bridge_settings.country[0] == '\0') {
        return "(unset)";
    }
    cc[0] = bridge_settings.country[0];
    cc[1] = bridge_settings.country[1];
    cc[2] = '\0';
    return cc;
}

void console_dump_state(void)
{
#if BRIDGE_L2_BRIDGE
    /* A transparent bridge leases nothing and translates nothing, so there is no
     * pool and no flow table to report.  Saying so is the honest form: printing
     * "nat: 0/64 flows" would describe a bridge this build does not contain. */
    console_printf(ITF_MGMT,
                   "    profiles:   %u saved (active: %s)\n"
                   "    ssid:       %s\n"
                   "    pass:       %s\n"
                   "    country:    %s\n"
                   "    debug:      %s\n"
                   "    status:     %s\n"
                   /* Neither an address nor a gateway: a transparent bridge owns
                    * no IP at all, so the line says so instead of reporting a
                    * pair the station's own (stopped) client once learned. */
                   "    wifi IP:    %s\n"
                   "    host IPv4:  %s\n"
                   "    host IPv6:  %s\n"
                   "    mode:       transparent L2 bridge (no NAT, no lease)\n",
                   (unsigned)bridge_settings.count,
                   (bridge_settings.active == BRIDGE_NO_PROFILE) ? "none" : "active",
                   ssid_word(), pass_word(), country_word(),
                   bridge_settings.debug ? "on" : "off",
                   bridge_status_word(),
                   bridge_wifi_ipv4_word(),
                   bridge_host_ipv4_word(),
                   "(ipv6 off)");
#else
    console_printf(ITF_MGMT,
                   "    profiles:   %u saved (active: %s)\n"
                   "    ssid:       %s\n"
                   "    pass:       %s\n"
                   "    country:    %s\n"
                   "    debug:      %s\n"
                   "    status:     %s\n"
                   "    wifi IPv4:  %s  gw %s\n"
                   "    host IPv4:  %s\n"
                   "    host IPv6:  %s\n"
                   /* Not in the specification's tables: the NAT table is the
                    * bridge's hard limit on concurrent flows, and until now its
                    * occupancy was only visible on UART0, where nothing is
                    * plugged in.  A host that exhausts it sees silence, so the
                    * number belongs on the console that is actually open. */
                   "    nat:        %u/%u flows\n",
                   (unsigned)bridge_settings.count,
                   (bridge_settings.active == BRIDGE_NO_PROFILE) ? "none" : "active",
                   ssid_word(), pass_word(), country_word(),
                   bridge_settings.debug ? "on" : "off",
                   bridge_status_word(),
                   bridge_wifi_ipv4_word(), bridge_wifi_gateway_word(),
                   bridge_host_ipv4_word(),
                   /* Reported as it is: this build has LWIP_IPV6 = 0, so there
                    * is no IPv6 address to snoop for either side. */
                   "(ipv6 off)",
                   bridge_nat_active(), BRIDGE_NAT_TABLE_SIZE);
#endif
}


static void console_banner(void)
{
    console_printf(ITF_MGMT, "\r\n-- pico-usb-wifi v%s --\r\n", CONSOLE_VERSION);
}

static void console_prompt(void)
{
    console_write_str(ITF_MGMT, scan_menu ? "(back|join|scan|live) # "
                                          : "(set|scan|list|use|save) # ");
}

/* Echo the keystroke, keep the line, run it on Enter.  Backspace is handled so
 * a mistyped value can be fixed without retyping the whole line. */
static void console_char(char c)
{
    if ((c == '\r') || (c == '\n')) {
        console_write_str(ITF_MGMT, "\r\n");
        line[line_len] = '\0';
        console_exec();
        line_len = 0u;
        return;
    }
    if ((c == '\b') || (c == 0x7f)) {
        if (line_len > 0u) {
            line_len--;
            console_write_str(ITF_MGMT, "\b \b");
        }
        return;
    }
    if ((unsigned char)c < ' ') {
        return;                     /* other control bytes are ignored */
    }
    if (line_len + 1u >= sizeof(line)) {
        return;                     /* line full: drop the keystroke */
    }
    line[line_len++] = c;
    console_write(ITF_MGMT, &c, 1u);
}

/* --------------------------------- public API ---------------------------- */
void console_init(void)
{
    memset(&bridge_settings, 0, sizeof(bridge_settings));
    bridge_settings.active = BRIDGE_NO_PROFILE;
    bridge_settings.country[0] = 'X';
    bridge_settings.country[1] = 'X';

    if (settings_load(&bridge_settings)) {
        return;                     /* a provisioned board: use what is saved */
    }

    /* Nothing saved.  Seed the first profile from the build-time defaults so the
     * cmake -DBRIDGE_WIFI_SSID path keeps working next to the console. */
    if (strcmp(BRIDGE_WIFI_SSID, "CHANGE_ME_SSID") != 0) {
        bridge_settings.count  = 1u;
        bridge_settings.active = 0u;
        strncpy(bridge_settings.profile[0].ssid, BRIDGE_WIFI_SSID, BRIDGE_SSID_MAX_LEN);
        strncpy(bridge_settings.profile[0].pass, BRIDGE_WIFI_PASSWORD, BRIDGE_PASS_MAX_LEN);
    }
}

void console_poll(void)
{
    scan_service();

    if (!tud_cdc_n_ready(ITF_MGMT)) {
        return;                     /* console not enumerated yet */
    }
    if (!banner_shown) {
        banner_shown = true;
        console_banner();
        console_dump_state();
        console_prompt();
        console_flush(ITF_MGMT);
    }

    int32_t c;
    while ((c = tud_cdc_n_read_char(ITF_MGMT)) >= 0) {
        console_char((char)c);
    }
    console_flush(ITF_MGMT);
}
