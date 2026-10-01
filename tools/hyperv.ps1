# The ESP image boots first; the installed Omarchy disk (tools/omarchy-install.ps1), when present, rides
# along as a second disk for the netbooted Omarchy kernel to mount as root.
param([string]$Name = 'oh-my-uefpi', [string]$Switch = 'Default Switch', [int]$MemoryGB = 8, [int]$Cpus = 4,
      [int]$Width = 1920, [int]$Height = 1080,
      [string]$OmarchyDisk = 'oh-my-uefpi-omarchy.vhdx', [switch]$Replace)
$ErrorActionPreference = 'Stop'

$dir = (Get-VMHost).VirtualHardDiskPath
$src = (Resolve-Path (Join-Path $PSScriptRoot '..\build\oh-my-uefpi.vhdx')).Path
if (Get-VM -Name $Name -ErrorAction SilentlyContinue) {
    if (-not $Replace) { throw "VM '$Name' exists; pass -Replace to recreate it." }
    Stop-VM -Name $Name -TurnOff -Force -ErrorAction SilentlyContinue
    Remove-VM -Name $Name -Force
}
$disk = Join-Path $dir "$Name.vhdx"
Copy-Item $src $disk -Force
New-VM -Name $Name -Generation 2 -MemoryStartupBytes ($MemoryGB * 1GB) -VHDPath $disk -SwitchName $Switch | Out-Null
Set-VM -Name $Name -StaticMemory -ProcessorCount $Cpus -CheckpointType Disabled -AutomaticCheckpointsEnabled $false
# Single: the firmware and Linux's hyperv_drm both get exactly this mode (Omarchy's monitor is "preferred").
Set-VMVideo -VMName $Name -ResolutionType Single -HorizontalResolution $Width -VerticalResolution $Height
$esp = Get-VMHardDiskDrive -VMName $Name
$omarchy = Join-Path $dir $OmarchyDisk
if (Test-Path $omarchy) {
    Add-VMHardDiskDrive -VMName $Name -Path $omarchy
    "attached $omarchy"
}
Set-VMFirmware -VMName $Name -EnableSecureBoot Off -FirstBootDevice $esp
Start-VM -Name $Name
"started '$Name': vmconnect localhost $Name"
