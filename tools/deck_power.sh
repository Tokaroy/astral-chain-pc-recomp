#!/usr/bin/env bash
# Run a command with the ROG Ally limited to 14 W by default (about the Steam Deck TDP), then restore the previous limits.
# Also pins the command to 4 cores / 8 threads (the Deck has 4c/8t); the Ally's SMT siblings are N and N+8.
# Usage: tools/deck_power.sh [--watts N] [--cpus LIST] <command> [args...]
# Needs root to write the asus-armoury sysfs attributes (uses sudo).
set -u

# Use the Mesa build patched to behave like the Deck's GFX10.3 (RADV's DCC rule for MUTABLE_FORMAT images applied on GFX11 too),
# built in ~/src/mesa-deck from Mesa 26.2.6. MESA_DECK_ICD overrides the path; unset it to the empty string to use the system driver.
MESA_DECK_ICD=${MESA_DECK_ICD-$HOME/src/mesa-deck/share/vulkan/icd.d/radeon_icd.x86_64.json}
if [[ -n $MESA_DECK_ICD ]]; then
    if [[ -f $MESA_DECK_ICD ]]; then
        export VK_DRIVER_FILES=$MESA_DECK_ICD; echo "Vulkan driver: $MESA_DECK_ICD" >&2
        # The game gives its colour targets a format list by default on RDNA 2 under RADV only, which it tells from the device name
        # (runtime/vk_caps.c). This driver still reports PHOENIX, so the run asks for the list as a Deck gets it;
        # AC_TARGET_COMPRESSION=0 in the environment compares without.
        export AC_TARGET_COMPRESSION=${AC_TARGET_COMPRESSION-1}
    else echo "warning: $MESA_DECK_ICD not found, using the system driver" >&2; fi
fi

WATTS=14
CPUS=0-3,8-11
while [[ "${1:-}" == --* ]]; do
    case $1 in
        --watts) WATTS=$2 ;;
        --cpus) CPUS=$2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift 2
done
[[ $# -gt 0 ]] || { echo "usage: $0 [--watts N] [--cpus LIST] <command> [args...]" >&2; exit 2; }

DIR=/sys/class/firmware-attributes/asus-armoury/attributes
ATTRS=(ppt_pl1_spl ppt_pl2_sppt ppt_pl3_fppt)

[[ -d $DIR ]] || { echo "asus-armoury not available, running without a power limit" >&2; exec taskset -c "$CPUS" "$@"; }

declare -A OLD
for a in "${ATTRS[@]}"; do OLD[$a]=$(<"$DIR/$a/current_value"); done

setlimit() { echo "$2" | sudo ${SUDO_ASKPASS:+-A} tee "$DIR/$1/current_value" >/dev/null; }

restore() {
    for a in "${ATTRS[@]}"; do setlimit "$a" "${OLD[$a]}"; done
    echo "power limits restored: ${OLD[ppt_pl1_spl]}/${OLD[ppt_pl2_sppt]}/${OLD[ppt_pl3_fppt]} W" >&2
}
trap restore EXIT
trap 'exit 130' INT TERM HUP

sudo ${SUDO_ASKPASS:+-A} -v || exit 1
# Lower PL1 first, then PL2/PL3, so the firmware never sees PL1 > PL2. PL2 and PL3 have a minimum (15 W on the Ally):
# a lower value is refused and the boost limits stay at 43 / 53 W, so the clocks start high and fall once the boost
# window is spent (4.5 GHz down to 1.9 GHz in a minute at 13 W). Each limit is clamped to its minimum instead.
for a in "${ATTRS[@]}"; do
    w=$WATTS; min=$(<"$DIR/$a/min_value")
    (( w < min )) && w=$min
    setlimit "$a" "$w"
done
echo "power limits set to $(<"$DIR/ppt_pl1_spl/current_value")/$(<"$DIR/ppt_pl2_sppt/current_value")/$(<"$DIR/ppt_pl3_fppt/current_value") W (was ${OLD[ppt_pl1_spl]}/${OLD[ppt_pl2_sppt]}/${OLD[ppt_pl3_fppt]})" >&2

taskset -c "$CPUS" "$@"
