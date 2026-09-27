#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I "$root/components/novy" "$root/tests/controller_test.cpp" \
  "$root/components/novy/controller.cpp" -o "$build/controller_test"
"$build/controller_test"

# Stub only ESPHome interfaces; compile the real adapter and controller together.
for header in core/component.h core/automation.h core/hal.h core/log.h \
  components/binary_sensor/binary_sensor.h components/button/button.h \
  components/fan/fan.h components/light/light_output.h \
  components/remote_transmitter/remote_transmitter.h components/sensor/sensor.h \
  components/text_sensor/text_sensor.h components/api/api_server.h; do
  mkdir -p "$build/esphome/$(dirname "$header")"
  printf '#include "adapter_stubs.h"\n' > "$build/esphome/$header"
done
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -Wno-unused-variable -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I "$build" -I "$root/tests" -I "$root/components/novy" \
  "$root/tests/adapter_test.cpp" "$root/components/novy/novy.cpp" \
  "$root/components/novy/controller.cpp" -o "$build/adapter_test"
# ESPHome intentionally owns setup-time automations for the device lifetime.
ASAN_OPTIONS=detect_leaks=0 "$build/adapter_test"
