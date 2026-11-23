@echo off
rem Builds SprutmViewer.exe with the C# compiler of the .NET Framework 4 (part of Windows 10/11, nothing to install).
cd /d "%~dp0"
"%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:winexe /optimize+ /codepage:65001 ^
	/win32icon:sprutm.ico /out:SprutmViewer.exe ^
	/r:System.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll SprutmViewer.cs
if errorlevel 1 exit /b 1
echo SprutmViewer.exe built
