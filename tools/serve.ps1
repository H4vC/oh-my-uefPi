param([int]$Port = 8124, [string]$Switch = 'Default Switch')
$ErrorActionPreference = 'Stop'

# The VM finds this server as its DHCP gateway, which on an internal switch is the host's vEthernet address.
$ip = (Get-NetIPAddress -AddressFamily IPv4 -InterfaceAlias "vEthernet ($Switch)").IPAddress
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\serve')).Path
"serving $root on http://${ip}:$Port/"
python -m http.server $Port --bind $ip --directory $root
