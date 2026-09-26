param(
    [string]$Exe = 'C:\Users\zqw35\WorkBuddy\projects\ZDock\_review\ZDock_0.1.5.exe'
)
$ErrorActionPreference = 'Stop'
$out = 'C:\Users\zqw35\WorkBuddy\projects\ZDock\build\_review\_exe_version.txt'
$v = (Get-Item $Exe).VersionInfo
$lines = @()
$lines += 'path             = ' + $Exe
$lines += 'size             = ' + (Get-Item $Exe).Length
$lines += 'FileVersion      = ' + $v.FileVersion
$lines += 'ProductVersion   = ' + $v.ProductVersion
$lines += 'FileDescription  = ' + $v.FileDescription
$lines += 'ProductName      = ' + $v.ProductName
$lines += 'LegalCopyright   = ' + $v.LegalCopyright
$lines += 'CompanyName      = ' + $v.CompanyName
$lines | Out-File -FilePath $out -Encoding utf8
