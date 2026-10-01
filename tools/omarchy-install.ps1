# Installs Omarchy unattended onto oh-my-uefpi-omarchy.vhdx in Hyper-V's disk folder: a throwaway Gen2 VM boots
# the Omarchy ISO with build\cidata.iso (tools/omarchy-cidata.sh) as a second DVD. Disk first in the
# boot order: empty, it falls through to the ISO; after the install the VM reboots into Omarchy.
param([Parameter(Mandatory)][string]$Iso, [string]$Name = 'omarchy-install', [string]$Switch = 'Default Switch',
      [int]$DiskGB = 40, [int]$MemoryGB = 8, [int]$Cpus = 4)
$ErrorActionPreference = 'Stop'

$dir = (Get-VMHost).VirtualHardDiskPath
$disk = Join-Path $dir 'oh-my-uefpi-omarchy.vhdx'
$isoCopy = Join-Path $dir (Split-Path $Iso -Leaf)
$cidata = Join-Path $dir 'omarchy-cidata.iso'
if (Get-VM -Name $Name -ErrorAction SilentlyContinue) { throw "VM '$Name' exists; remove it first." }
if (Test-Path $disk) { throw "$disk exists; remove it to reinstall." }
if (-not (Test-Path $isoCopy)) { Copy-Item $Iso $isoCopy }
Copy-Item (Join-Path $PSScriptRoot '..\build\cidata.iso') $cidata -Force

New-VHD -Path $disk -SizeBytes ($DiskGB * 1GB) -Dynamic | Out-Null
New-VM -Name $Name -Generation 2 -MemoryStartupBytes ($MemoryGB * 1GB) -VHDPath $disk -SwitchName $Switch | Out-Null
Set-VM -Name $Name -StaticMemory -ProcessorCount $Cpus -CheckpointType Disabled -AutomaticCheckpointsEnabled $false
$dvd = Add-VMDvdDrive -VMName $Name -Path $isoCopy -Passthru
Add-VMDvdDrive -VMName $Name -Path $cidata
Set-VMFirmware -VMName $Name -EnableSecureBoot Off -BootOrder (Get-VMHardDiskDrive -VMName $Name), $dvd
Start-VM -Name $Name
"installing in '$Name' onto ${disk}: vmconnect localhost $Name"
