# Installs the SPRUT-M viewers for the current user into %LOCALAPPDATA%\Programs\SPRUT-M:
#   "SPRUT-M Видео"           - app\SprutmVideo.exe, the native viewer (no browser, lowest delay)
#   "SPRUT-M Видео (браузер)" - SprutmViewer.exe, the web page in an Edge / Chrome app window (spare)
# with shortcuts on the desktop and in the Start menu. Run: powershell -ExecutionPolicy Bypass -File install.ps1
$ErrorActionPreference = 'Stop'
$dir = Join-Path $env:LOCALAPPDATA 'Programs\SPRUT-M'
New-Item -ItemType Directory -Force $dir | Out-Null
$running = Get-Process SprutmVideo, SprutmViewer -ErrorAction SilentlyContinue
if ($running) { $running | Stop-Process -Force; $running | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue }
# a just-closed program keeps its exe locked for a moment: retry the copy for up to 5 s
foreach ($f in @(@('app\SprutmVideo.exe', 'SprutmVideo.exe'), @('SprutmViewer.exe', 'SprutmViewer.exe'))) {
	for ($i = 0; ; $i++) {
		try { Copy-Item (Join-Path $PSScriptRoot $f[0]) (Join-Path $dir $f[1]) -Force -ErrorAction Stop; break }
		catch { if ($i -ge 19) { throw }; Start-Sleep -Milliseconds 250 }
	}
}
# files from a downloaded zip carry the "from the internet" mark: SmartScreen would ask at every start
Get-ChildItem $dir -Filter *.exe | Unblock-File

# the native viewer on the fast graphics card (Windows "Graphics settings": high performance). Measured on a laptop
# with Intel + RTX 4070: 8.8 ms from a field arriving to the screen on the RTX, 11.0 ms on the Intel.
$gp = 'HKCU:\Software\Microsoft\DirectX\UserGpuPreferences'
if (-not (Test-Path $gp)) { New-Item $gp -Force | Out-Null }
Set-ItemProperty $gp -Name (Join-Path $dir 'SprutmVideo.exe') -Value 'GpuPreference=2;'

$sh = New-Object -ComObject WScript.Shell
foreach ($folder in @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Programs'))) {
	foreach ($app in @(
		@{ Name = 'SPRUT-M Видео'; Exe = 'SprutmVideo.exe'; Desc = 'Видео с платы SPRUT-M (Raspberry Pi по кабелю LAN), без браузера' },
		@{ Name = 'SPRUT-M Видео (браузер)'; Exe = 'SprutmViewer.exe'; Desc = 'Видео с платы SPRUT-M: веб-страница в окне Edge / Chrome' })) {
		$exe = Join-Path $dir $app.Exe
		$lnk = $sh.CreateShortcut((Join-Path $folder ($app.Name + '.lnk')))
		$lnk.TargetPath = $exe
		$lnk.WorkingDirectory = $dir
		$lnk.IconLocation = "$exe,0"
		$lnk.Description = $app.Desc
		$lnk.Save()
	}
}
Write-Output "installed to $dir"
