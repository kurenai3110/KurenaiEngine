@echo off
rem ---------------------------------------------------------------------------
rem Launches Sample3D with the DX11 backend. Double-click from Explorer is fine.
rem
rem This file is intentionally ASCII-only, and it pins the console code page to
rem 932 before calling Run.bat. cmd.exe reads a batch file while tracking a byte
rem offset (goto seeks by byte offset too) and interprets those bytes using the
rem console code page, so Run.bat -- which contains Japanese -- must be read
rem under the same code page it is stored in (CP932). Without this, inheriting a
rem UTF-8 console desynchronizes the parser and Run.bat breaks apart mid-token.
rem The chcp is safe to do here because this file has nothing multi-byte in it.
rem
rem All work (configuration selection, pre-flight checks, logging) is in Run.bat.
rem Extra arguments such as -debug / -release are passed straight through.
rem ---------------------------------------------------------------------------
for /f "tokens=2 delims=:" %%C in ('chcp') do set "KURENAI_ORIGINAL_CP=%%C"
set "KURENAI_ORIGINAL_CP=%KURENAI_ORIGINAL_CP: =%"
chcp 932 >nul

call "%~dp0Run.bat" %*
set "KURENAI_EXIT_CODE=%ERRORLEVEL%"

chcp %KURENAI_ORIGINAL_CP% >nul
set "KURENAI_ORIGINAL_CP="
exit /b %KURENAI_EXIT_CODE%
