@echo off
REM Script de desarrollo — configura MSVC y lanza Tauri dev
setlocal

set MSVC_VER=14.44.35207
set MSVC_BIN=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\%MSVC_VER%\bin\Hostx64\x64
set WINSDK_BIN=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64

set PATH=%MSVC_BIN%;%WINSDK_BIN%;%USERPROFILE%\.cargo\bin;%PATH%

set LIB=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\%MSVC_VER%\lib\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\ucrt\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64

set INCLUDE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\%MSVC_VER%\include;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\ucrt;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared

set CARGO_TARGET_DIR=C:\tmp\esp32-target

cd /d "%~dp0"
npm run tauri dev
