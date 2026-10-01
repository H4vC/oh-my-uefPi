# oh-my-uefpi

A single `BOOTX64.EFI` that boots a Hyper-V VM straight into a chat prompt,
streams replies from an OpenAI-compatible `/v1/chat/completions` endpoint
(DeepSeek by default), and boots whatever the model is asked to: Omarchy,
netboot.xyz, or any UEFI image at an http(s) URL. No OS, no history. Dressed
up like omp, for the memes.

```
  ═══╦═══╦═══   oh-my-uefpi
     ║   ║      boot-to-prompt, no os
     ║   ╚══
  • url     https://api.deepseek.com/v1/chat/completions
  • model   deepseek-v4-pro
  • net     172.24.69.196
  • tls     ca.der 2 roots
  • dns     api.deepseek.com 3.173.21.63 via 1.1.1.1

❯ Who are you?
I am oh-my-uefpi, a UEFI boot app. I can chainload a .efi image from a URL you give me, nothing else.

❯ boot omarchy
```

`/boot <url> [args]` skips the model. `/exit` or Ctrl-D powers off.

## Run it

Build in WSL (Ubuntu 24.04), deploy from an elevated PowerShell:

```
wsl sudo apt install gcc make gnu-efi qemu-utils mtools dosfstools fdisk openssl systemd-ukify systemd-boot-efi genisoimage jq
cp config.example.txt config.txt   # then set key=sk-...
wsl make                           # build/oh-my-uefpi.vhdx: the app, config.txt and ca.der on an ESP
wsl make serve                     # serve/netboot.xyz.efi
tools\serve.ps1                    # HTTP on the Default Switch address, port 8124, over serve\
tools\hyperv.ps1 -Replace          # Gen2 VM oh-my-uefpi: 8 GB, 4 vCPUs, 1920x1080, Secure Boot off
vmconnect localhost oh-my-uefpi
```

`ca.der` (built from `CA_PEM`: ISRG Root X1 and Amazon Root CA 1) holds the TLS
roots. DeepSeek V4 models think by default and then reject tool rounds unless
their `reasoning_content` is sent back, so `config.txt` sets
`thinking=disabled`.

## Omarchy

Installed once, unattended, onto `oh-my-uefpi-omarchy.vhdx`, which
`tools\hyperv.ps1` attaches as the VM's second disk. "boot omarchy" loads that
system's own UKI from `serve\omarchy.efi` and lands on the Hyprland desktop
(user `omarchy`, password `omarchy`, autologin) about 20 s later.

```
wsl make omarchy-iso cidata                 # hash-checked 4.0.4 ISO; build/cidata.iso: full-disk config + SSH key
tools\omarchy-install.ps1 -Iso <printed>    # throwaway VM installs (~1 min) and reboots into Omarchy
tools\omarchy-pull.ps1 -Address <its IP>    # autologin; its UKI, cmdline embedded, to serve\omarchy.efi
```

Then remove that VM, keeping the disk. Rerun the pull after a kernel update on
the installed system. The embedded cmdline drops `resume=`: it names the disk
as `/dev/sda`, but under oh-my-uefpi it is the second disk and the boot would
wait for a device that never appears.

## How it works

- **Agent:** the system prompt is the oh-my-uefpi identity plus the Omarchy and
  netboot.xyz URLs, whose host is the DHCP gateway (the Hyper-V host, so the
  Default Switch's changing subnet needs no config). One tool,
  `boot(url, args)`, downloads an image into memory and chainloads it with
  `LoadImage`/`StartImage`; `args` become LoadOptions. Failures go back to the
  model as tool results, up to 4 rounds. The image gets the NIC's device path
  plus a URI node, as firmware HTTP boot does; iPXE needs that `DeviceHandle`.
- **TLS in the app:** Hyper-V's firmware TLS sends no SNI, and CloudFront
  (DeepSeek) refuses it, so `src/net.c` does HTTP/1.1 itself over the
  firmware's `EFI_TCP4`, with [BearSSL](https://bearssl.org) 0.6 (fetched into
  `vendor/`, hash-checked) for TLS. Entropy: `EFI_RNG_PROTOCOL` or RDRAND;
  certificate time: `GetTime()`.
- **DNS:** the Default Switch's DNS never answers the firmware, so lookups race
  the DHCP servers, 1.1.1.1 and 8.8.8.8.
- **Stalls:** an overloaded DeepSeek holds the stream open with keep-alive
  comments; after 90 s without a token the request is abandoned.
- **Shutdown:** Hyper-V's "Shut Down" needs integration services; use `/exit`,
  Ctrl-D or "Turn Off".
