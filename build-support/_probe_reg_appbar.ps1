$ErrorActionPreference = 'SilentlyContinue'
Write-Output "=== HKCU Explorer 下与 AppBar/Tray 相关的键 ==="
Get-ChildItem 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer' -Recurse -Depth 1 |
  Where-Object { $_.PSChildName -match 'AppBar|Tray|StuckRect|DeskBand' } |
  ForEach-Object { Write-Output $_.Name }
Write-Output ""
Write-Output "=== StuckRects3 (任务栏位置) ==="
$k = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StuckRects3'
if (Test-Path -LiteralPath $k) {
  $v = (Get-ItemProperty -LiteralPath $k).Settings
  Write-Output ("Settings bytes = " + (($v | ForEach-Object { $_.ToString('X2') }) -join ' '))
}
