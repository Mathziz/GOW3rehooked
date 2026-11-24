@echo off
rem Starts gow3.exe with its diagnostics on (see Reporting bugs in the README). Arguments are passed on.
start "" "%~dp0gow3.exe" --debug %*
