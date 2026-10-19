@echo off
rem The small tools that check the shared code on their own, outside the game.
setlocal
cd /d "%~dp0"
if not defined VCINSTALLDIR call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist bin mkdir bin
if not exist obj\tools mkdir obj\tools
cl /nologo /O2 /EHsc /std:c++17 /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Fo"obj\tools\\" /Fe"bin\shadertable_check.exe" ^
    tools\shadertable_check.cpp common\shader_table_build.cpp common\shader_table.cpp common\lzma_decode.cpp || exit /b 1
