#!/usr/bin/env bash
#
# Baut PicoBoyGB für den alten PicoBoy Color (RP2040, 2 MB Flash).
#
# Aufruf:   ./build.sh                    bauen
#           ./build.sh clean              Bauordner löschen und neu bauen
#           ./build.sh -DCPU_KHZ=200000   CMake-Optionen durchreichen
#
# Ergebnis: build/picoboygb.uf2
#
# Pico SDK: PICO_SDK_PATH, sonst ~/.cache/pbcp-sdks/pico-sdk (2.1.0, wie bei
# den PBC+-Projekten), sonst ~/pico/pico-sdk.
#
set -euo pipefail
HIER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "${1:-}" = clean ]; then
    rm -rf "${HIER}/build"
    shift
fi

if [ -z "${PICO_SDK_PATH:-}" ]; then
    for d in "${HOME}/.cache/pbcp-sdks/pico-sdk" "${HOME}/pico/pico-sdk"; do
        if [ -f "${d}/pico_sdk_init.cmake" ]; then PICO_SDK_PATH="$d"; break; fi
    done
fi
if [ -z "${PICO_SDK_PATH:-}" ]; then
    echo "FEHLER: Pico SDK nicht gefunden, bitte PICO_SDK_PATH setzen." >&2
    exit 1
fi
export PICO_SDK_PATH

GEN=()
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)

cmake -S "${HIER}" -B "${HIER}/build" "${GEN[@]}" \
      -DPICO_SDK_PATH="${PICO_SDK_PATH}" \
      -DPICO_PLATFORM=rp2040 -DPICO_BOARD=pico "$@"
cmake --build "${HIER}/build" -j"$(nproc 2>/dev/null || echo 2)"

echo
echo "Fertig: ${HIER}/build/picoboygb.uf2      (Waveshare-Geraet)"
echo "        ${HIER}/build/picoboygb_pbc.uf2  (PicoBoy Color)"
echo "Flashen: BOOTSEL halten, USB anstecken, Datei auf RPI-RP2 kopieren."
