@echo off
setlocal

rem Stable entry point for machines whose PowerShell execution policy blocks
rem direct .ps1 launch. Bypass applies only to this child process and does not
rem change the user's or machine's policy.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash_jtag_fresh.ps1" %*
exit /b %ERRORLEVEL%
