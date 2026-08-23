#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
busybox=${BUSYBOX_DIR:-"$repo/third_party/busybox"}
seed="$repo/usr/busybox/extron.config"
port_patch="$repo/usr/busybox/busybox-1.38.0-extron.patch"

if ! grep -q '__extron__' "$busybox/include/platform.h"; then
    patch -d "$busybox" -p1 < "$port_patch"
fi

CCACHE_DISABLE=1 make -C "$busybox" allnoconfig >/dev/null

while IFS= read -r setting; do
    case "$setting" in
        CONFIG_*=n)
            key=${setting%%=*}
            sed -i \
                -e "s|^$key=.*\$|# $key is not set|" \
                "$busybox/.config"
            ;;
        CONFIG_*=y|CONFIG_*=\"*\"|CONFIG_*=*)
            key=${setting%%=*}
            sed -i \
                -e "s|^# $key is not set\$|$setting|" \
                -e "s|^$key=.*\$|$setting|" \
                "$busybox/.config"
            ;;
    esac
done < "$seed"

CCACHE_DISABLE=1 make -C "$busybox" oldconfig </dev/null >/dev/null
