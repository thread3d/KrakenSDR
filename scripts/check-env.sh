#!/bin/bash
# check-env.sh -- report what this host gives us to build and run against.
#
# Covers: platform, toolchain, the SoapySDR / librtlsdr stack, the build
# dependencies ogn-rf needs, GPU/FFT availability, and the sudo/credential
# situation that decides HOW dependencies get installed.
#
# Read-only. Exit code 0 always (it is a report, not a gate) unless the host
# is missing something fundamental, in which case 1.

set -u

have() { command -v "$1" >/dev/null 2>&1; }

head1() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }
ok()   { printf '  \033[32m%-24s\033[0m %s\n' "$1" "$2"; }
bad()  { printf '  \033[31m%-24s\033[0m %s\n' "$1" "$2"; }
inf()  { printf '  %-24s %s\n' "$1" "$2"; }

missing=0

head1 "platform"
inf "hostname"   "$(hostname)"
inf "kernel"     "$(uname -sr)"
inf "arch"       "$(uname -m)"
inf "distro"     "$(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"
if [ -f /etc/nv_tegra_release ]; then
  inf "L4T"      "$(head -1 /etc/nv_tegra_release | cut -c3-)"
else
  inf "L4T"      "(not a Tegra device)"
fi
inf "nproc"      "$(nproc)"
inf "mem"        "$(free -h 2>/dev/null | awk '/^Mem:/{print $2}')"
inf "disk free"  "$(df -h . 2>/dev/null | awk 'NR==2{print $4" on "$6}')"

head1 "toolchain"
for t in gcc g++ make cmake git python3; do
  if have "$t"; then
    case "$t" in
      gcc|g++|python3) v=$("$t" --version 2>&1 | head -1) ;;
      cmake|git)       v=$("$t" --version 2>&1 | head -1) ;;
      make)            v=$("$t" --version 2>&1 | head -1) ;;
    esac
    ok "$t" "$v"
  else
    bad "$t" "NOT FOUND"
    missing=$((missing+1))
  fi
done

head1 "SDR stack"
if have SoapySDRUtil; then
  ok "SoapySDRUtil" "$(SoapySDRUtil --info 2>/dev/null | awk -F': ' '/Lib Version/{print $2}')"
  moddir=$(SoapySDRUtil --info 2>/dev/null | awk -F': ' '/Module found/{print $2; exit}')
  inf "modules" "$(SoapySDRUtil --info 2>/dev/null | grep -c 'Module found') found"
  if SoapySDRUtil --info 2>/dev/null | grep -q rtlsdrSupport; then
    ok "SoapySDR rtlsdr" "present"
  else
    bad "SoapySDR rtlsdr" "MISSING -- no RTL-SDR via Soapy"
  fi
else
  bad "SoapySDRUtil" "NOT FOUND -- install soapysdr-tools"
fi

if have rtl_test; then
  ok "rtl_test" "$(command -v rtl_test)"
else
  bad "rtl_test" "NOT FOUND -- install rtl-sdr (tools)"
fi

lib=$(ldconfig -p 2>/dev/null | awk '/librtlsdr\.so/{print $NF; exit}')
if [ -n "$lib" ]; then ok "librtlsdr runtime" "$lib"; else bad "librtlsdr runtime" "MISSING"; fi

head1 "ogn-rf build dependencies"
# ogn-rf needs: librtlsdr (bias-T fork), libconfig, fftw3, libjpeg
check_hdr() {  # name header pkg
  if [ -f "$2" ]; then ok "$1" "$2"
  else bad "$1" "MISSING ($2) -- package $3"; ognf_missing=1; fi
}
ognf_missing=0
check_hdr "librtlsdr headers" "/usr/include/rtl-sdr.h"        "librtlsdr-dev"
check_hdr "libconfig headers"  "/usr/include/libconfig.h"      "libconfig-dev"
check_hdr "fftw3 headers"      "/usr/include/fftw3.h"          "libfftw3-dev"
check_hdr "libjpeg headers"    "/usr/include/jpeglib.h"        "libjpeg-turbo8-dev"

for lib in libconfig fftw3 fftw3f jpeg pthread rt; do
  if ldconfig -p 2>/dev/null | grep -q "lib${lib}\.so"; then
    ok "lib${lib}" "linkable"
  else
    bad "lib${lib}" "not in ldconfig"
    ognf_missing=1
  fi
done

head1 "GPU / FFT"
if have nvidia-smi; then
  inf "nvidia-smi" "$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1)"
else
  inf "nvidia-smi" "(absent)"
fi
if [ -d /opt/vc/src/hello_pi/hello_fft ]; then
  inf "VideoCore FFT" "present -- ogn-rf Makefile would try USE_RPI_GPU_FFT"
  inf "warning" "VideoCore code does NOT work on Tegra; force CPU/FFTW3"
else
  ok "VideoCore FFT" "absent (correct for Tegra; FFTW3 will be used)"
fi
for c in cuda-11 cuda-12 cuda; do
  [ -d "/usr/local/$c" ] && inf "cuda" "/usr/local/$c"
done

head1 "privileges / credentials"
if sudo -n true 2>/dev/null; then
  ok "passwordless sudo" "available -- apt install possible"
else
  bad "passwordless sudo" "NOT available -- apt install needs a password"
fi
inf "groups" "$(id -nG 2>/dev/null)"
command -v gh >/dev/null 2>&1 && ok "gh" "$(gh --version 2>/dev/null | head -1)" \
                               || inf "gh" "absent (pushes are done from the dev box)"
inf "git identity" "$(git config --global user.email 2>/dev/null || echo '(unset)')"

head1 "verdict"
if [ "$ognf_missing" -ne 0 ]; then
  echo "  ogn-rf CANNOT be built yet: build dependencies are missing."
  echo
  echo "  Two ways forward:"
  echo "    a) ask for the sudo password once:"
  echo "         sudo apt install librtlsdr-dev libconfig-dev"
  echo "    b) build them into a local prefix with no root (see docs/03):"
  echo "         scripts/build-deps.sh <prefix>"
  echo
fi
if [ "$missing" -ne 0 ]; then
  echo "  $missing fundamental tool(s) missing."
  exit 1
fi
exit 0
