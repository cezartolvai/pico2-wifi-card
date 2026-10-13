/* ---------------------------------------------------------------------------
 * console.h - the management console (first CDC-ACM function, /dev/ttyACM0)
 * and the debug stream (second CDC-ACM function, /dev/ttyACM1).
 *
 * See openrouter-console-design.md.bak for the specification this implements.
 * ------------------------------------------------------------------------ */
#ifndef PICO2_WIFI_CARD_CONSOLE_H
#define PICO2_WIFI_CARD_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

#define BRIDGE_PROFILE_MAX       8
#define BRIDGE_SSID_MAX_LEN    32
#define BRIDGE_PASS_MAX_LEN    63
#define BRIDGE_NO_PROFILE      0xffu

/* One Wi-Fi credential set.  An empty pass means an open network. */
typedef struct {
    char ssid[BRIDGE_SSID_MAX_LEN + 1];
    char pass[BRIDGE_PASS_MAX_LEN + 1];
} wifi_profile_t;

/* Everything the console can change at runtime, and everything save/restore
 * has to round-trip through flash. */
typedef struct {
    wifi_profile_t profile[BRIDGE_PROFILE_MAX];
    uint8_t  count;                       /* profiles in use, 0..8          */
    uint8_t  active;                      /* index into profile[], or NO_.. */
    char     country[3];                  /* "XX" = worldwide, else 2 chars */
    bool     debug;                       /* debug stream on / off          */
} bridge_settings_t;

extern bridge_settings_t bridge_settings;

/* Load the saved record (or seed the first profile from the build-time
 * defaults when flash is empty), then print the banner on ttyACM0. */
void console_init(void);

/* Read the console, run commands, print state.  Call from the poll loop. */
void console_poll(void);

/* True when the debug stream (ttyACM1) should be written to. */
bool console_debug_enabled(void);

/* NULL when nothing is configured yet - the bridge must not then try to
 * associate, because there is no network to join. */
const wifi_profile_t *console_active_profile(void);

/* cyw43 country code for the configured country code. */
uint32_t console_country_code(void);

/* One line on the debug stream.  Dropped silently when debug is off. */
void console_debug_line(const char *line);

/* printf-style line on the debug stream. */
void console_debug_logf(const char *fmt, ...);

/* A recovery report.  Written even when the debug stream is off: a crash that
 * fixed itself still has to leave a trace, and this is the only way to see it
 * without wiring a UART. */
void console_recovered_line(const char *line);

/* True when at least one CDC-ACM console exists (device configured and the
 * function has endpoints).  The recovery report is held until this is true. */
bool console_reachable(void);

/* True while the console is in live-scan mode (disassociated, streaming the
 * access points it sees).  The LED uses it for its double-flash pattern. */
bool console_scan_live(void);


/* The full state dump, on the management console. */
void console_dump_state(void);

#endif /* PICO2_WIFI_CARD_CONSOLE_H */
