param([int]$vk = 13)
Add-Type -Namespace W -Name U32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern uint MapVirtualKey(uint vk, uint mapType);
'@
$p = Get-Process boreaz -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -eq $p) { Write-Output "no boreaz process"; exit }
$h = $p.MainWindowHandle
$scan = [int][W.U32]::MapVirtualKey([uint32]$vk, 0)
$down = [IntPtr](1 -bor ($scan -shl 16))
$up   = [IntPtr](1 -bor ($scan -shl 16) -bor 0x40000000 -bor 0x80000000)
$null = [W.U32]::PostMessage($h, 0x0100, [IntPtr]$vk, $down)
Start-Sleep -Milliseconds 90
$null = [W.U32]::PostMessage($h, 0x0101, [IntPtr]$vk, $up)
Write-Output "posted vk=$vk scan=$scan hwnd=$h"
