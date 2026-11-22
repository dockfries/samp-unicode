@echo off
rem Builds and runs the offline checks. No game, no D3D, no Python needed.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
pushd "%~dp0"
set RC=0
for %%T in (config_defaults_test serveraddr_test) do (
    cl /nologo /EHsc /W4 /std:c++17 /Fe:%%T.exe %%T.cpp >build.log || (type build.log & set RC=1 & goto :done)
    echo.
    %%T.exe || set RC=1
)
:done
popd
exit /b %RC%
