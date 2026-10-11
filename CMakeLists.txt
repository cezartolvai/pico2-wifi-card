cmake_minimum_required(VERSION 3.13)

# Raspberry Pi Pico 2 W  (RP2350 + CYW43 Wi-Fi/BT).
# This has to happen before pico_sdk_import.cmake: the SDK reads PICO_BOARD while
# processing project(), so setting it afterwards would leave the build on the
# default (rp2040, no Wi-Fi).
set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(PICO_PLATFORM rp2350 CACHE STRING "Chip type")

include(pico_sdk_import.cmake)

project(pico2_wifi_card C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico2_wifi_card)

target_sources(pico2_wifi_card PRIVATE
    src/main.c
    src/bridge.c
    src/console.c
    src/diagnostics.c
    src/nat.c
    src/usb_descriptors.c
    # TinyUSB's tiny lwIP DHCP server (MIT, Sergey Fetisov) - see src/dhcp_notes.md
    ${PICO_TINYUSB_PATH}/lib/networking/dhserver.c
)

target_include_directories(pico2_wifi_card PRIVATE
    src
    ${PICO_TINYUSB_PATH}/lib/networking
)

target_link_libraries(pico2_wifi_card PRIVATE
    pico_stdlib
    pico_cyw43_arch_lwip_poll     # cyw43 + lwIP (NO_SYS=1) + poll async_context
    pico_unique_id
    tinyusb_device
    tinyusb_board
    hardware_watchdog               # the recovery watchdog in src/diagnostics.c
)


# The knobs in src/config.h are #ifndef-guarded so they can be overridden from
# the cmake command line.  A -D on the cmake command line only creates a CMake
# cache variable - it is NOT passed to the compiler - so every knob has to be
# forwarded here.  Without this block, -DBRIDGE_WIFI_SSID=... silently does
# nothing and the image keeps the placeholder from config.h.
set(BRIDGE_KNOBS
    BRIDGE_WIFI_ENABLE
    BRIDGE_WIFI_SSID
    BRIDGE_WIFI_PASSWORD
    BRIDGE_WIFI_AUTH
    BRIDGE_WIFI_COUNTRY
    BRIDGE_WIFI_RETRY_SECONDS
    BRIDGE_WIFI_AUTH_TRANSITION
    BRIDGE_HOST_NET_PREFIX
    BRIDGE_HOST_IP_LAST
    BRIDGE_HOST_NETMASK
    BRIDGE_HOST_POOL_FIRST
    BRIDGE_HOST_POOL_SIZE
    BRIDGE_HOST_LEASE_SECONDS
    BRIDGE_HOST_DOMAIN
    BRIDGE_HOST_MTU
    BRIDGE_NAT_ENABLE
    BRIDGE_NAT_TABLE_SIZE
    BRIDGE_NAT_PORT_BASE
    BRIDGE_NAT_TCP_IDLE_MS
    BRIDGE_NAT_UDP_IDLE_MS
    BRIDGE_NAT_ICMP_IDLE_MS
    BRIDGE_FALLBACK_DNS
    BRIDGE_USB_RX_SLOTS
    BRIDGE_MAX_FRAME
    BRIDGE_VERBOSE
    BRIDGE_STATUS_INTERVAL_MS
    BRIDGE_LED
    BRIDGE_LED_TICK_MS
    BRIDGE_WATCHDOG_MS
    BRIDGE_RECOVERY_HOLD_MS
    BRIDGE_SELFTEST
    TRACE_FRAMES
)


foreach(knob IN LISTS BRIDGE_KNOBS)
    if(DEFINED ${knob})
        target_compile_definitions(pico2_wifi_card PRIVATE "${knob}=${${knob}}")
    endif()
endforeach()

# The USB port carries the CDC-NCM network interface plus two CDC-ACM console
# functions, so the mirror of the console also lives on UART0: GPIO0 = TX,
# GPIO1 = RX, 115200 8N1.
pico_enable_stdio_uart(pico2_wifi_card 1)
pico_enable_stdio_usb(pico2_wifi_card 0)

pico_add_extra_outputs(pico2_wifi_card)
