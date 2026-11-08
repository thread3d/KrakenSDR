#!/bin/bash
# check-device.sh -- is a KrakenSDR attached to this host, and how?
#
# Reports the KrakenSDR's expected USB signature:
#   * N x Realtek RTL2838  (0bda:2838) -- the receive chains
#   * 1 x CP210x serial    (10c4:*   ) -- optional; some revisions switch the
#                                        noise source over RTL GPIO instead
# plus the internal hub they sit behind, the /dev nodes, and the serial numbers.
#
# Exit codes: 0 = at least one RTL2838 found, 1 = no RTL2838, 2 = wrong host.

set -u

RTL_VIDPID="0bda:2838"
EXPECTED_CHANNELS=5

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
amber() { printf '\033[33m%s\033[0m\n' "$*"; }
head1() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }

head1 "host"
printf 'hostname : %s\n' "$(hostname)"
printf 'kernel   : %s\n' "$(uname -sr)"
printf 'arch     : %s\n' "$(uname -m)"

head1 "RTL2838 receive chains ($RTL_VIDPID)"
if ! command -v lsusb >/dev/null 2>&1; then
  red "lsusb not found -- install usbutils"
  exit 2
fi

rtl_lines=$(lsusb -d "$RTL_VIDPID" 2>/dev/null)
if [ -z "$rtl_lines" ]; then
  rtl_count=0
else
  rtl_count=$(printf '%s\n' "$rtl_lines" | wc -l | tr -d ' ')
fi

printf 'found %s of %s expected\n' "$rtl_count" "$EXPECTED_CHANNELS"

# Per-dongle detail straight from sysfs: USB path and EEPROM serial.
# The serials are what HeIMDALL and rtl_tcp use to pin a channel to a tuner,
# and their enumeration order is NOT the same as the channel numbering.
head1 "per-tuner detail (sysfs)"
found_paths=""
for d in /sys/bus/usb/devices/*/; do
  [ -f "$d/idVendor" ] || continue
  [ "$(cat "$d/idVendor"  2>/dev/null)" = "0bda" ] || continue
  [ "$(cat "$d/idProduct" 2>/dev/null)" = "2838" ] || continue
  name=$(basename "$d")
  serial=$(cat "$d/serial"  2>/dev/null || echo '-')
  prod=$(cat   "$d/product" 2>/dev/null || echo '-')
  spd=$(cat    "$d/speed"   2>/dev/null || echo '-')
  found_paths="$found_paths $name"
  printf '  %-10s serial=%-6s speed=%-6s %s\n' "$name" "$serial" "$spd" "$prod"
done
[ -n "$found_paths" ] || printf '  (none)\n'

head1 "internal hub"
# The KrakenSDR's five tuners sit behind one internal hub on a single USB port.
hub_parent=$(printf '%s\n' $found_paths | sed 's/\.[0-9]*$//' | sort -u | head -1)
if [ -n "${hub_parent:-}" ]; then
  hp=$(cat "/sys/bus/usb/devices/$hub_parent/idVendor"  2>/dev/null || echo '?')
  hq=$(cat "/sys/bus/usb/devices/$hub_parent/idProduct" 2>/dev/null || echo '?')
  hprod=$(cat "/sys/bus/usb/devices/$hub_parent/product" 2>/dev/null || echo '?')
  printf 'parent port %-8s id=%s:%s  %s\n' "$hub_parent" "$hp" "$hq" "$hprod"
else
  printf '  (no common parent found)\n'
fi

head1 "serial bridge (CP210x)"
cp=$(lsusb -d 10c4: 2>/dev/null)
if [ -n "$cp" ]; then
  printf '%s\n' "$cp"
  green "CP210x present"
else
  amber "no CP210x found"
  printf '  This is normal for revisions that switch the calibration noise source\n'
  printf '  over the RTL2832U GPIOs (see heimdall rtl_daq.c: rtlsdr_set_bias_tee_gpio)\n'
  printf '  rather than over a serial port.\n'
fi
printf 'serial nodes: '
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | tr '\n' ' ' || true
printf '\n'

head1 "kernel-side view"
if command -v rtl_test >/dev/null 2>&1; then
  rtl_test -t 2>&1 | sed 's/^/  /' | head -20
else
  printf '  rtl_test not installed; using SoapySDR instead\n'
  if command -v SoapySDRUtil >/dev/null 2>&1; then
    SoapySDRUtil --find='driver=rtlsdr' 2>&1 \
      | grep -E 'rtl =|serial =|tuner =|Found device' | sed 's/^/  /'
  else
    printf '  SoapySDRUtil not installed either\n'
  fi
fi

head1 "verdict"
if [ "$rtl_count" -eq 0 ]; then
  red "NO KrakenSDR receive chains found -- device not attached, not powered, or hub fault"
  printf '\nChecklist:\n'
  printf '  1. known-good USB 3 cable and a port that can supply the current\n'
  printf '  2. prefer a direct port or a POWERED hub; this host already has nested hubs\n'
  printf '  3. sudo dmesg -w while plugging in; look for -71 (signal) / over-current\n'
  printf '  4. ./watch-usb.sh 120  to watch it enumerate\n'
  exit 1
fi

if [ "$rtl_count" -lt "$EXPECTED_CHANNELS" ]; then
  amber "only $rtl_count of $EXPECTED_CHANNELS tuners -- array is incomplete, no coherent DoA"
  exit 1
fi

green "all $EXPECTED_CHANNELS tuners present -- array is complete"
exit 0
