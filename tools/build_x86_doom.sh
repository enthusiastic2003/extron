#!/bin/bash
# Cross-build DOOM (doomgeneric + usr/doom/doomgeneric_extron.c) against the mlibc
# sysroot with the compiler wrapper. Output: build-x86_64/doom/doom, plus the
# shareware WAD cached in build-x86_64/extlibs/dl, both installed into the root
# disk by tools/build_x86_rootfs.sh (/usr/bin/doom, /usr/share/games/doom/doom1.wad).
#   tools/build_x86_doom.sh        # REBUILD=1 to rebuild every object
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
UL="${EXTRON_USERLAND:-$REPO/build-x86_64/userland}"
DL="$REPO/build-x86_64/extlibs/dl"
B="$REPO/build-x86_64/doom"
REV=e257a09527062bfb986b6e5e0780839b6cbd5b71
SHA=26f8f60daa6c6e4d3c01e5eac9fb700d592dede803d13ac848154ff356b2a2e7
WAD_URL="https://github.com/Daivuk/PureDOOM/raw/master/doom1.wad"
WAD_SHA=1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771   # doom1.wad v1.9 shareware
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
export EXTRON_SYSROOT="$UL/sysroot"
CC="$REPO/tools/x86_64-extron-gcc"
[ -f "$EXTRON_SYSROOT/usr/lib/libc.so" ] || { echo "build the userland first" >&2; exit 1; }

mkdir -p "$DL" "$B"
TAR="$DL/doomgeneric-$REV.tar.gz"
[ -f "$TAR" ] || curl -fsSL "https://github.com/ozkl/doomgeneric/archive/$REV.tar.gz" -o "$TAR"
echo "$SHA  $TAR" | sha256sum -c - >/dev/null
SRC="$B/doomgeneric-$REV/doomgeneric"
[ -d "$SRC" ] || tar -xzf "$TAR" -C "$B"

# The shareware episode (freely redistributable) that doom needs at run time.
WAD="$DL/doom1.wad"
[ -f "$WAD" ] || curl -fsSL "$WAD_URL" -o "$WAD"
echo "$WAD_SHA  $WAD" | sha256sum -c - >/dev/null

# doomgeneric's own sources minus the other platforms' doomgeneric_*.c.
SOURCES="dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main
 d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick
 i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls
 m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights
 p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch
 p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things
 sha1 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff w_checksum
 w_file w_main w_wad z_zone w_file_stdc i_input i_video doomgeneric"
CFLAGS="-O2 -w -I$SRC -I$REPO/usr/doom -DNORMALUNIX -DLINUX"

[ "${REBUILD:-0}" = 1 ] && rm -rf "$B/obj"
mkdir -p "$B/obj"
compile() {
    local src="$1" obj="$2"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then "$CC" $CFLAGS -c "$src" -o "$obj"; fi
}
export -f compile; export CC CFLAGS
{
    for f in $SOURCES; do echo "$SRC/$f.c $B/obj/$f.o"; done
    echo "$REPO/usr/doom/doomgeneric_extron.c $B/obj/doomgeneric_extron.o"
} | xargs -P "$JOBS" -L 1 bash -c 'compile "$0" "$1"' \
    || { echo "doom: compile failed" >&2; exit 1; }

OBJS=""; for f in $SOURCES doomgeneric_extron; do OBJS="$OBJS $B/obj/$f.o"; done
"$CC" $OBJS -lm -o "$B/doom"
strip --strip-unneeded "$B/doom" 2>/dev/null || true
echo "doom: $(stat -c %s "$B/doom") bytes, WAD $(stat -c %s "$WAD") bytes"
