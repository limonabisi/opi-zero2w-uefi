# Build a ready-to-boot Windows 10/11 ARM64 disk image for the Orange Pi Zero 2W.
#
# Windows is applied into a fixed VHD on your PC's own disk (fast), made
# bootable from USB, then the whole image is written to the USB drive in one
# sequential pass with balenaEtcher or Rufus. Much faster than applying
# Windows directly onto a USB stick.
#
# Run in an ADMIN PowerShell:
#   Set-ExecutionPolicy -Scope Process Bypass
#   .\make-windows-image.ps1 -Iso "C:\path\Win10_Arm64.iso"
#
# Options:
#   -Edition "Windows 10 Pro"   edition name inside the ISO (default; part of a name also works)
#   -SizeGB 16                  image size; extend C: later in Disk Management
#   -Out "C:\opi-windows.img"   output file (needs SizeGB of free space)
#   -User opi                   local account created on first boot (no password)

param(
  [Parameter(Mandatory = $true)] [string] $Iso,
  [string] $Edition = "Windows 10 Pro",
  [int]    $SizeGB  = 16,
  [string] $Out     = "$PSScriptRoot\opi-windows.img",
  [string] $User    = "opi"
)

$ErrorActionPreference = "Stop"

function Step($m) { Write-Host "`n==> $m" -ForegroundColor Cyan }
function Run($exe, $argList) {
  & $exe @argList
  if ($LASTEXITCODE -ne 0) { throw "$exe failed ($LASTEXITCODE)" }
}
function FreeLetters {
  $used = (Get-PSDrive -PSProvider FileSystem).Name
  $free = @('R','S','T','U','V','W','X','Y','Z') | Where-Object { $used -notcontains $_ }
  if ($free.Count -lt 2) { throw "need two free drive letters (R-Z)" }
  return $free[0], $free[1]
}

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
      [Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw "Run this script in an administrator PowerShell."
}

$Iso = (Resolve-Path $Iso).Path
$vhd = [IO.Path]::ChangeExtension($Out, ".vhd")
if (Test-Path $vhd) { throw "$vhd already exists, delete it first." }
if (Test-Path $Out) { throw "$Out already exists, delete it first." }

# ---------------------------------------------------------------- ISO ---
Step "Mounting the ISO"
$mount = Mount-DiskImage -ImagePath $Iso -PassThru
$isoDrive = ($mount | Get-Volume).DriveLetter + ":"
try {
  $wim = @("$isoDrive\sources\install.wim", "$isoDrive\sources\install.esd") | Where-Object { Test-Path $_ } | Select-Object -First 1
  if (-not $wim) { throw "no install.wim / install.esd in the ISO" }

  $images = Get-WindowsImage -ImagePath $wim
  $images | ForEach-Object { Write-Host ("  {0}: {1}" -f $_.ImageIndex, $_.ImageName) }
  $pick = $images | Where-Object { $_.ImageName -eq $Edition } | Select-Object -First 1
  if (-not $pick) {
    $pick = $images | Where-Object { $_.ImageName -like "*$Edition*" -and $_.ImageName -notlike "* N" } | Select-Object -First 1
  }
  if (-not $pick) { throw "edition '$Edition' not found, use -Edition with one of the names above" }
  $info = Get-WindowsImage -ImagePath $wim -Index $pick.ImageIndex
  Write-Host ("  using {0}: {1} ({2})" -f $pick.ImageIndex, $pick.ImageName, $info.Architecture)
  if ($info.Architecture -ne 12) { Write-Warning "this image is not ARM64 (architecture $($info.Architecture)), it will not boot on the board" }

  # -------------------------------------------------------------- VHD ---
  Step "Creating a $SizeGB GB disk image"
  $S, $W = FreeLetters
  $dp = @"
create vdisk file="$vhd" maximum=$($SizeGB * 1024) type=fixed
select vdisk file="$vhd"
attach vdisk
convert gpt
create partition efi size=260
format quick fs=fat32 label="SYSTEM"
assign letter=$S
create partition msr size=16
create partition primary
format quick fs=ntfs label="Windows"
assign letter=$W
"@
  $dpFile = New-TemporaryFile
  Set-Content -Path $dpFile -Value $dp -Encoding ASCII
  Run diskpart @("/s", $dpFile)

  try {
    # ------------------------------------------------------ Windows ---
    Step "Applying Windows (compact) - this is the long part"
    Run dism @("/Apply-Image", "/ImageFile:$wim", "/Index:$($pick.ImageIndex)", "/ApplyDir:${W}:\", "/Compact")

    Step "Boot files"
    Run bcdboot @("${W}:\Windows", "/s", "${S}:", "/f", "UEFI")
    $bcd = "${S}:\EFI\Microsoft\Boot\BCD"
    Run bcdedit @("/store", $bcd, "/set", "{default}", "recoveryenabled", "no")
    Run bcdedit @("/store", $bcd, "/set", "{default}", "bootstatuspolicy", "IgnoreAllFailures")

    Step "Boot from USB (Windows To Go settings)"
    Run reg @("load", "HKLM\OPIZ", "${W}:\Windows\System32\config\SYSTEM")
    try {
      Run reg @("add", "HKLM\OPIZ\ControlSet001\Control", "/v", "PortableOperatingSystem", "/t", "REG_DWORD", "/d", "1", "/f")
      foreach ($svc in 'usbstor','UASPStor','usbehci','usbohci','usbhub','usbccgp','USBHUB3') {
        if (Test-Path "HKLM:\OPIZ\ControlSet001\Services\$svc") {
          Run reg @("add", "HKLM\OPIZ\ControlSet001\Services\$svc", "/v", "BootFlags", "/t", "REG_DWORD", "/d", "4", "/f")
        }
      }
    } finally {
      [gc]::Collect()
      Start-Sleep 1
      reg unload HKLM\OPIZ | Out-Null
    }

    Step "First boot: local account '$User', no online account screens"
    New-Item -ItemType Directory -Force "${W}:\Windows\Panther" | Out-Null
    @"
<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend">
  <settings pass="oobeSystem">
    <component name="Microsoft-Windows-International-Core" processorArchitecture="arm64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
      <InputLocale>041f:0000041f</InputLocale>
      <SystemLocale>tr-TR</SystemLocale>
      <UILanguage>tr-TR</UILanguage>
      <UserLocale>tr-TR</UserLocale>
    </component>
    <component name="Microsoft-Windows-Shell-Setup" processorArchitecture="arm64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
      <OOBE>
        <HideEULAPage>true</HideEULAPage>
        <HideOEMRegistrationScreen>true</HideOEMRegistrationScreen>
        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>
        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>
        <HideLocalAccountScreen>true</HideLocalAccountScreen>
        <ProtectYourPC>3</ProtectYourPC>
        <NetworkLocation>Home</NetworkLocation>
      </OOBE>
      <UserAccounts>
        <LocalAccounts>
          <LocalAccount wcm:action="add">
            <Name>$User</Name>
            <Group>Administrators</Group>
            <Password><Value></Value><PlainText>true</PlainText></Password>
          </LocalAccount>
        </LocalAccounts>
      </UserAccounts>
      <AutoLogon>
        <Enabled>true</Enabled>
        <Username>$User</Username>
        <Password><Value></Value><PlainText>true</PlainText></Password>
        <LogonCount>9999999</LogonCount>
      </AutoLogon>
    </component>
  </settings>
</unattend>
"@ | Set-Content -Path "${W}:\Windows\Panther\unattend.xml" -Encoding UTF8
  } finally {
    Step "Detaching the disk image"
    Set-Content -Path $dpFile -Value "select vdisk file=`"$vhd`"`ndetach vdisk" -Encoding ASCII
    diskpart /s $dpFile | Out-Null
    Remove-Item $dpFile -ErrorAction SilentlyContinue
  }
} finally {
  Dismount-DiskImage -ImagePath $Iso | Out-Null
}

# A fixed VHD is the raw disk plus a 512-byte footer at the end, so it can be
# written to a USB drive as a plain .img file.
Rename-Item $vhd $Out
Step "Done: $Out"
Write-Host "Write it to the USB drive with balenaEtcher (or Rufus, DD image mode)."
Write-Host "On the board: ESC at boot -> choose the USB drive."
