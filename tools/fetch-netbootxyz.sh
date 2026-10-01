#!/bin/sh
# netboot.xyz: a ~1 MB iPXE EFI app whose menu netboots dozens of OS installers.
set -eu
URL=https://github.com/netbootxyz/netboot.xyz/releases/download/3.0.3/netboot.xyz.efi
mkdir -p "$(dirname "$1")"
curl -fsSL -o "$1.part" "$URL"
echo "56bb21e9f6d79eadb947e7031a088c9afcc48aa1eb99b2502d161f0b1c586a82  $1.part" | sha256sum -c --quiet
mv "$1.part" "$1"
