#!/bin/bash
# Builds $1 (a "cidata" ISO) that makes the Omarchy ISO install itself, unattended, onto the first
# Hyper-V SCSI disk (/dev/sda) of $2 bytes. The JSON is the 4.0.4 configurator's full-disk output.
# User and password: $OMARCHY_USER / $OMARCHY_PASSWORD (default omarchy/omarchy); SSH key: $3.pub.
set -euo pipefail
out=$1 disk_bytes=$2 key=$3
user=${OMARCHY_USER:-omarchy} password=${OMARCHY_PASSWORD:-omarchy}
mib=$((1024 * 1024)) gib=$((1024 * 1024 * 1024))
boot_start=$mib boot_size=$((2 * gib))
main_start=$((boot_start + boot_size))
main_size=$((disk_bytes / mib * mib - main_start - mib))

[[ -f $key ]] || ssh-keygen -t ed25519 -N "" -q -C uefpi -f "$key"
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
hash=$(openssl passwd -6 "$password")

cat >"$dir/user_credentials.json" <<EOF
{
    "root_enc_password": $(jq -Rn --arg v "$hash" '$v'),
    "users": [ { "enc_password": $(jq -Rn --arg v "$hash" '$v'), "groups": [], "sudo": true, "username": "$user" } ]
}
EOF

part() { # size start fs flags mountpoint btrfs mount_options obj_id
	cat <<EOF
{
    "btrfs": $6, "dev_path": null, "flags": $4, "fs_type": "$3", "mount_options": $7, "mountpoint": $5,
    "obj_id": "$8", "status": "create", "type": "primary",
    "size": { "sector_size": { "unit": "B", "value": 512 }, "unit": "B", "value": $1 },
    "start": { "sector_size": { "unit": "B", "value": 512 }, "unit": "B", "value": $2 }
}
EOF
}
subvols='[ { "mountpoint": "/", "name": "@" }, { "mountpoint": "/home", "name": "@home" },
           { "mountpoint": "/var/log", "name": "@log" }, { "mountpoint": "/var/cache/pacman/pkg", "name": "@pkg" } ]'

cat >"$dir/user_configuration.json" <<EOF
{
    "app_config": null,
    "archinstall-language": "English",
    "auth_config": {},
    "audio_config": { "audio": "pipewire" },
    "bootloader_config": { "bootloader": "Limine", "uki": false, "removable": false },
    "custom_commands": [],
    "omarchy_install": {
        "mode": "full_disk",
        "defer_provisioning": false,
        "target_mount": "/mnt",
        "boot": { "esp_mount": "/boot", "esp_path": "/EFI/limine", "efi_binary": "limine_x64.efi", "enable_fallback": true },
        "storage": { "kernel": "linux-omarchy" }
    },
    "disk_config": {
        "config_type": "default_layout",
        "device_modifications": [ {
            "device": "/dev/sda",
            "partitions": [
                $(part $boot_size $boot_start fat32 '[ "boot", "esp" ]' '"/boot"' '[]' '[]' ea21d3f2-82bb-49cc-ab5d-6f81ae94e18d),
                $(part $main_size $main_start btrfs '[]' null "$subvols" '[ "compress=zstd" ]' 8c2c2b92-1070-455d-b76a-56263bab24aa)
            ],
            "wipe": true
        } ]
    },
    "hostname": "omarchy",
    "kernels": [ "linux-omarchy" ],
    "network_config": { "type": "iso" },
    "ntp": true,
    "parallel_downloads": 8,
    "script": null,
    "services": [],
    "swap": true,
    "timezone": "UTC",
    "locale_config": { "kb_layout": "us", "sys_enc": "UTF-8", "sys_lang": "en_US.UTF-8" },
    "mirror_config": {
        "custom_repositories": [],
        "custom_servers": [
            {"url": "https://mirror.omarchy.org/\$repo/os/\$arch"},
            {"url": "https://mirror.rackspace.com/archlinux/\$repo/os/\$arch"},
            {"url": "https://geo.mirror.pkgbuild.com/\$repo/os/\$arch"}
        ],
        "mirror_regions": {},
        "optional_repositories": []
    },
    "packages": [ "base-devel", "git", "omarchy-keyring", "omarchy-settings", "omarchy" ],
    "profile_config": { "gfx_driver": null, "greeter": null, "profile": {} },
    "version": "3.0.9"
}
EOF
jq empty "$dir/user_configuration.json" "$dir/user_credentials.json"
echo "Omarchy" >"$dir/user_full_name.txt"
echo "omarchy@example.com" >"$dir/user_email_address.txt"
echo false >"$dir/user_encrypt_installation.txt"
cp "$key.pub" "$dir/authorized_keys"
mkdir -p "$(dirname "$out")"
genisoimage -quiet -output "$out" -volid cidata -joliet -rock "$dir"
