#!/bin/bash
# Configure + build the USB-only bisection variant (no cyw43, no NAT).
# It lives in its own build dir so build/ (the real target) stays untouched.
set -e
cd "$(dirname "$0")/.."
cmake -S . -B build-usbonly -G Ninja \
      -DPICO_SDK_PATH="$PWD/.pico-sdk" \
      -DBRIDGE_WIFI_ENABLE=0 \
      -DBRIDGE_NAT_ENABLE=0
ninja -C build-usbonly
echo "usb-only build ok: build-usbonly/pico2_wifi_card.uf2"
