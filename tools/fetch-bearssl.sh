#!/bin/sh
set -eu
TGZ=https://bearssl.org/bearssl-0.6.tar.gz
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/bearssl.tgz" "$TGZ"
echo "6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14  $tmp/bearssl.tgz" | sha256sum -c --quiet
tar xzf "$tmp/bearssl.tgz" -C "$tmp"
mkdir -p "$(dirname "$1")"
rm -rf "$1"
mv "$tmp/bearssl-0.6" "$1"
