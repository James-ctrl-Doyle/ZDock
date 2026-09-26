$ErrorActionPreference = 'Stop'
$out = 'C:\Users\zqw35\WorkBuddy\projects\ZDock\build\_review\_exe_version.txt'
$f = 'C:\Users\zqw35\WorkBuddy\projects\ZDock\_review\ZDock_0.1.4.exe'
$items = @(
    (Get-Item $f).VersionInfo
)
$lines = @()
$lines += 'path           = ' + $f
$lines += 'size           = ' + (Get-Item $f).Length
$lines += 'FileVersion    = ' + $items[0].FileVersion
$lines += 'ProductVersion = ' + $items[0].ProductVersion
$lines += 'FileDescription= ' + $items[0].FileDescription
$lines += 'ProductName    = ' + $items[0].ProductName
$lines += 'LegalCopyright = ' + $items[0].LegalCopyright
$lines += 'CompanyName    = ' + $items[0].CompanyName
$lines += 'OriginalFilename = ' + $items[0].OriginalFilename
$lines | Out-File -FilePath $out -Encoding utf8
