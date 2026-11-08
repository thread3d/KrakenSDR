#!/bin/bash
# watch-usb.sh -- watch the USB bus for a KrakenSDR enumerating.
#
# Usage: ./watch-usb.sh [seconds]      (default 120)
#
# Prints a line per USB add/bind event, filtering to the interesting ones, and
# then reports whether the RTL2838 chains showed up. Useful when the device is
# flaky: an add followed by a remove is a power or cable fault, while no add at
# all is a dead cable, port or device.

set -u
DURATION="${1:-120}"
RTL_VIDPID="0bda:2838"

if ! command -v udevadm >/dev/null 2>&1; then
  echo "udevadm not found -- cannot watch udev events" >&2
  exit 2
fi

before=$(lsusb -d "$RTL_VIDPID" 2>/dev/null | wc -l | tr -d ' ')
echo "Watching USB for ${DURATION}s (RTL2838 chains before: ${before})"
echo "Plug the KrakenSDR in now..."
echo

timeout "$DURATION" udevadm monitor --udev --subsystem-match=usb --property 2>/dev/null \
  | awk '
      /^ACTION=/  { act = substr($0, 8) }
      /^DEVPATH=/ { path = substr($0, 9) }
      /^PRODUCT=/ { prod = substr($0, 9) }
      /^ID_VENDOR_ID=/ {
        vid = substr($0, 14)
        if (act == "add" || act == "bind")
          printf "%-6s %-58s %s\n", act, path, prod
      }
    ' \
  | grep -Ev '/(usb[0-9]+|[0-9]+-[0-9]+)$' \
  | head -60

echo
after=$(lsusb -d "$RTL_VIDPID" 2>/dev/null | wc -l | tr -d ' ')
echo "RTL2838 chains after: ${after} (was ${before})"

if [ "$after" -ge 5 ]; then
  echo "OK: full 5-channel array present"
  exit 0
elif [ "$after" -gt "$before" ]; then
  echo "PARTIAL: some tuners appeared ($after of 5) -- power or hub problem likely"
  exit 1
elif [ "$after" -eq 0 ]; then
  echo "NONE: nothing enumerated. Check cable, port and power; if it enumerated"
  echo "      and then vanished, look at 'sudo dmesg | tail' for -71 / over-current."
  exit 1
else
  echo "UNCHANGED"
  exit 1
fi
