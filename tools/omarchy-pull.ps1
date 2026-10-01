# Copies the installed Omarchy's UKI off the running system and serves it as serve\omarchy.efi with its
# kernel command line embedded. Rerun after a kernel update on that system.
param([string]$Address = 'omarchy.mshome.net', [string]$User = 'omarchy', [string]$Password = 'omarchy',
      [string]$Key = "$HOME\.ssh\uefpi_omarchy")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
# BatchMode: fail instead of waiting at an invisible password prompt.
$opts = "-i `"$Key`" -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o LogLevel=ERROR"
# make cidata generates the key in WSL; Windows OpenSSH wants a copy only this user can read.
if (-not (Test-Path $Key)) {
    New-Item -ItemType Directory -Force (Split-Path $Key) | Out-Null
    wsl cat ~/.ssh/uefpi_omarchy | Set-Content -Encoding ascii $Key
    icacls $Key /inheritance:r /grant:r "${env:USERNAME}:F" | Out-Null
}

# Sent on stdin (LF only): PowerShell 5 mangles quotes in native-command arguments. Also turns on SDDM
# autologin so "boot omarchy" lands on the desktop.
$remote = Join-Path $env:TEMP 'omarchy-pull.sh'
[IO.File]::WriteAllText($remote, (@(
    "echo '$Password' | sudo -S -p '' sh -c 'printf ""[Autologin]\nUser=$User\nSession=hyprland-uwsm\n"" > /etc/sddm.conf.d/zz-autologin.conf'",
    "echo '$Password' | sudo -S -p '' install -m644 /boot/EFI/Linux/omarchy_linux-omarchy.efi /tmp/omarchy.efi",
    "echo '$Password' | sudo -S -p '' cat /boot/limine.conf | sed -n 's/^ *cmdline: //p' | head -1"
) -join "`n") + "`n")
$cmdline = (cmd /c "ssh $opts $User@$Address bash -s < `"$remote`"") -join ' '
Remove-Item $remote

# Limine's cmdline minus resume=: under oh-my-uefpi this disk is not /dev/sda, and a missing resume
# device stalls the boot.
$cmdline = ($cmdline -split '\s+' | Where-Object { $_ -and $_ -notmatch '^resume(_offset)?=' }) -join ' '
if ($cmdline -notmatch 'root=') { throw "no root= in Limine cmdline: '$cmdline'" }
cmd /c "scp $opts ${User}@${Address}:/tmp/omarchy.efi `"$root\build\omarchy-installed.efi`""
if ($LASTEXITCODE) { throw 'scp failed' }
wsl --cd $root tools/omarchy-uki.sh build/omarchy-installed.efi "$cmdline" serve/omarchy.efi
if ($LASTEXITCODE) { throw 'ukify failed' }
"serve\omarchy.efi: $cmdline"
