#!/bin/sh
# Rebuilds the installed system's UKI ($1) as $3 with kernel command line $2 embedded, so the boot tool
# needs no args: systemd-stub uses .cmdline when LoadOptions are empty.
set -eu
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
objcopy -O binary --only-section=.linux "$1" "$tmp/linux"
objcopy -O binary --only-section=.initrd "$1" "$tmp/initrd"
ukify build --linux "$tmp/linux" --initrd "$tmp/initrd" --cmdline "$2" --output "$tmp/out.efi" >/dev/null
mkdir -p "$(dirname "$3")"
mv "$tmp/out.efi" "$3"
