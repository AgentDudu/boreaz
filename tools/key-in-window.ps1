# One key, or one string of characters, into the Boreaz window. See key-in-window.md for why this
# is how the keyboard is measured: the window has no way to be driven any other way in a script.
param([int]$vk = 13, [string]$text = '')
Add-Type -Namespace W -Name U32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern uint MapVirtualKey(uint vk, uint mapType);
'@
$p = Get-Process boreaz -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -eq $p) { Write-Output "no boreaz process"; exit }
$h = $p.MainWindowHandle

if ($text -ne '') {
    # WM_CHAR, which is where GLFW hands a character to an ImGui text field. Typed one at a time with
    # a gap, because a frame is 16 ms and every character has to be seen by one of them.
    foreach ($ch in $text.ToCharArray()) {
        $null = [W.U32]::PostMessage($h, 0x0102, [IntPtr][int]$ch, [IntPtr]0)
        Start-Sleep -Milliseconds 25
    }
    Write-Output "typed $($text.Length) characters into hwnd=$h"
    exit
}

$scan = [int][W.U32]::MapVirtualKey([uint32]$vk, 0)
$down = [IntPtr](1 -bor ($scan -shl 16))
$up   = [IntPtr](1 -bor ($scan -shl 16) -bor 0x40000000 -bor 0x80000000)
$null = [W.U32]::PostMessage($h, 0x0100, [IntPtr]$vk, $down)
Start-Sleep -Milliseconds 90
$null = [W.U32]::PostMessage($h, 0x0101, [IntPtr]$vk, $up)
Write-Output "posted vk=$vk scan=$scan hwnd=$h"
