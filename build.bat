@echo off
rem Build samp-unicode.asi through the same project file CI uses.
rem Run inside "x86 Native Tools Command Prompt for VS".
rem
rem This used to call cl/link directly with its own flags and then link every
rem obj\*.obj in the folder, which could pick up stale objects and drift from the
rem CI build. One build path only: msbuild samp-unicode.vcxproj.
setlocal
if not defined VCINSTALLDIR (
  echo [!] Run this from the x86 Native Tools Command Prompt for Visual Studio
  exit /b 1
)

msbuild samp-unicode.vcxproj /nologo /m /v:minimal /p:Configuration=Release /p:Platform=Win32
if errorlevel 1 exit /b 1

rem Keep the historical layout: the .asi next to the sources, where the install
rem instructions and the harness expect it.
copy /y "bin\Release\samp-unicode.asi" "samp-unicode.asi" >nul
if errorlevel 1 (
  echo [!] msbuild succeeded but bin\Release\samp-unicode.asi is missing
  exit /b 1
)

echo.
echo [OK] samp-unicode.asi generated
