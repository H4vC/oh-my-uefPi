#!/bin/sh
# Downloads the Omarchy 4.0.4 ISO to $1 (hash-checked) for tools/omarchy-install.ps1.
set -eu
VER=4.0.4
SHA=ddeded2758c48318d201dfdac905ecb28f570441883f0c052ea3cd5d05acf92d
echo "$SHA  $1" | sha256sum -c --quiet 2>/dev/null && exit 0
mkdir -p "$(dirname "$1")"
curl -fL -o "$1.part" "https://iso.omarchy.org/omarchy-$VER.iso"
echo "$SHA  $1.part" | sha256sum -c --quiet
mv "$1.part" "$1"
