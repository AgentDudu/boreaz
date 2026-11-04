# The keyboard walk a folder search is made of: open the explorer (F3), type a drive root in the
# address bar (F4), then hop folder to folder with the filter (F5 + a few letters + Enter) until a
# song loads. The claim it measures is the one in docs/pr-folder-search.md: the loaded song's name
# appears on stdout, which is only possible if every hop worked. See tools/key-in-window.md for why
# keys have to be posted. Not part of the build.
#
# It assumes this machine's layout: the song ends up at <repo>/build/drive-ab.mid, reached by
# filtering Users / <this account> / VSC_Projects / boreaz / build / .mid from C:\. The account
# name is read from the environment rather than written here, so the script does not commit
# whoever's machine it was measured on. Change the list for another machine.
param([switch]$short)
$ErrorActionPreference = 'Stop'
$repo = (Get-Location).Path
Add-Type -Namespace W -Name U32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern uint MapVirtualKey(uint vk, uint mapType);
'@
function Key([int]$vk) {
    $scan = [int][W.U32]::MapVirtualKey([uint32]$vk, 0)
    $extended = @(0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2D, 0x2E)
    $ext = if ($extended -contains $vk) { 0x01000000 } else { 0 }
    $down = [IntPtr](1 -bor ($scan -shl 16) -bor $ext)
    $up = [IntPtr]($down -bor 0x40000000 -bor 0x80000000)
    $null = [W.U32]::PostMessage($h, 0x0100, [IntPtr]$vk, $down)
    Start-Sleep -Milliseconds 90
    $null = [W.U32]::PostMessage($h, 0x0101, [IntPtr]$vk, $up)
    Start-Sleep -Milliseconds 120
}
function SendText([string]$s) {
    foreach ($ch in $s.ToCharArray()) {
        $null = [W.U32]::PostMessage($h, 0x0102, [IntPtr][int]$ch, [IntPtr]0)
        Start-Sleep -Milliseconds 25
    }
}
$exe = Join-Path $repo 'build\boreaz.exe'
$out = Join-Path $repo 'build\_scratch\walk.out.txt'
$err = Join-Path $repo 'build\_scratch\walk.err.txt'
$p = Start-Process -FilePath $exe -ArgumentList '--gui' -WorkingDirectory $repo `
    -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
Start-Sleep -Seconds 4
$h = $p.MainWindowHandle
Write-Output "hwnd=$h"

Key 0x72   # F3: the explorer
Start-Sleep -Milliseconds 400
Key 0x73   # F4: the address bar takes the keyboard
    SendText 'C:/'
Key 0x0D   # Enter in the field: go to the drive root
Start-Sleep -Milliseconds 900
Write-Output 'jumped to C:/'
if (-not $short) {
    $account = $env:USERNAME
    if (-not $account) { throw 'USERNAME is not in the environment; this walk needs the account folder name' }
    foreach ($name in @('Users', $account, 'VSC_Projects', 'boreaz', 'build', '.mid')) {
        Key 0x74           # F5: the filter takes the keyboard
        SendText $name
        Key 0x0D           # Enter takes the top row the filter left
        Start-Sleep -Milliseconds 700
        Write-Output "hopped through $name"
    }
}
Start-Sleep -Seconds 2
$null = [W.U32]::PostMessage($h, 0x0100, [IntPtr]0x1B, [IntPtr]1)   # Escape closes the explorer
$null = [W.U32]::PostMessage($h, 0x0101, [IntPtr]0x1B, [IntPtr]0xC0000000)
Start-Sleep -Milliseconds 400
# A note, to prove the keyboard came back to the instrument after the fields had it.
Key 0x52   # r = F#
Start-Sleep -Milliseconds 600
$null = [W.U32]::PostMessage($h, 0x0112, [IntPtr]0xF060, [IntPtr]0)  # WM_CLOSE
Start-Sleep -Seconds 2
if (-not $p.HasExited) { $p.Kill() }
Write-Output '=== stdout ==='
Get-Content $out -ErrorAction SilentlyContinue
Write-Output '=== stderr ==='
Get-Content $err -ErrorAction SilentlyContinue
