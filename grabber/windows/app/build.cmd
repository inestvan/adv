@echo off
rem Builds SprutmVideo.exe with the C# compiler of the .NET Framework 4 (part of Windows 10/11, nothing to install).
cd /d "%~dp0"
"%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:winexe /optimize+ /platform:x64 /codepage:65001 ^
	/unsafe- /win32icon:..\sprutm.ico /win32manifest:app.manifest /out:SprutmVideo.exe ^
	/r:System.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll /r:System.Web.Extensions.dll /r:System.Core.dll ^
	Program.cs MainForm.cs Video.cs Shaders.cs Net.cs Gl.cs
if errorlevel 1 exit /b 1
echo SprutmVideo.exe built
