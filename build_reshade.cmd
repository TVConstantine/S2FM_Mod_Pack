@echo off
rem The ReShade tool: reshade\bin\s2fm_reshade_core.dll (ReShade 6.8.0's runtime and effect compiler, the core),
rem reshade\bin\rs_harness.exe (the core's tests) and reshade\bin\s2fm_reshade.dll (the tool's window, an engine
rem tool on Qt 4.8.3 like the game's). Nothing goes into the game here; installer\s2fm_mods.ps1 does that. Needs
rem third_party\reshade from "python tools\reshade_sdk.py" and third_party\qt4 from "python tools\qt4_sdk.py".
setlocal
cd /d "%~dp0"
if not defined VCINSTALLDIR call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set RS=third_party\reshade
set QTSRC=third_party\qt-everywhere-opensource-src-4.8.3
set QT4=third_party\qt4
set OBJ=reshade\obj
set BIN=reshade\bin
if not exist "%RS%\source\runtime.cpp" (
    echo third_party\reshade is missing: run "python tools\reshade_sdk.py" first.
    exit /b 1
)
if not exist "%QT4%\bin\moc.exe" (
    echo third_party\qt4 is missing: run "python tools\qt4_sdk.py" first.
    exit /b 1
)
if not exist %OBJ%\harness mkdir %OBJ%\harness
if not exist %OBJ%\tool mkdir %OBJ%\tool
if not exist %BIN% mkdir %BIN%

rem --- The core.
rem version.h, which ReShade's own build writes from its git tag (tools\update_version.ps1).
(
echo #pragma once
echo #define VERSION_FULL 6.8.0.0
echo #define VERSION_MAJOR 6
echo #define VERSION_MINOR 8
echo #define VERSION_REVISION 0
echo #define VERSION_BUILD 0
echo #define VERSION_STRING_FILE "6.8.0.0"
echo #define VERSION_STRING_PRODUCT "6.8.0 S2FM"
) > %OBJ%\version.h

rem The shaders the runtime loads from its own module.
fxc /nologo /T ps_4_0 /E main /Fo %OBJ%\copy_ps.cso %RS%\res\shaders\copy_ps.hlsl >nul || exit /b 1
fxc /nologo /T vs_4_0 /E main /Fo %OBJ%\fullscreen_vs.cso %RS%\res\shaders\fullscreen_vs.hlsl >nul || exit /b 1
fxc /nologo /T cs_5_0 /E main /Fo %OBJ%\mipmap_cs_5_0.cso %RS%\res\shaders\mipmap_cs_5_0.hlsl >nul || exit /b 1
copy /Y reshade\core\rs_core.rc %OBJ%\rs_core.rc >nul
pushd %OBJ%
rc /nologo /I ..\..\%RS%\res /fo rs_core.res rs_core.rc || (popd & exit /b 1)
popd

set DEFS=/DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /DNDEBUG /D_HAS_EXCEPTIONS=0 /D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS /D_CRT_SECURE_NO_WARNINGS /DSTBI_NO_STDIO /DSTBI_WRITE_NO_STDIO /DFPNG_NO_STDIO /DUTF_CPP_CPLUSPLUS=201703L
set INC=/I%OBJ% /I%RS%\res /I%RS%\source /I%RS%\include /I%RS%\deps\stb /I%RS%\deps\stb_image /I%RS%\deps\fpng\src /I%RS%\deps\jxl_simple_lossless /I%RS%\deps\utfcpp\source /Ireshade\core /Ireshade
set CXX=cl /nologo /c /MP /O2 /Zi /MT /EHsc /std:c++17 /utf-8 /W1 %DEFS% %INC% /Fd%OBJ%\rs_core_cl.pdb /Fo%OBJ%\

set RSSRC=%RS%\source\runtime.cpp %RS%\source\runtime_api.cpp %RS%\source\effect_codegen_dxbc.cpp %RS%\source\effect_expression.cpp %RS%\source\effect_lexer.cpp %RS%\source\effect_parser_exp.cpp %RS%\source\effect_parser_stmt.cpp %RS%\source\effect_preprocessor.cpp %RS%\source\effect_symbol_table.cpp %RS%\source\ini_file.cpp %RS%\source\dll_log.cpp %RS%\source\dll_resources.cpp %RS%\source\input.cpp %RS%\source\platform_utils_windows.cpp %RS%\source\d3d11\d3d11_impl_command_list.cpp %RS%\source\d3d11\d3d11_impl_command_queue.cpp %RS%\source\d3d11\d3d11_impl_device.cpp %RS%\source\d3d11\d3d11_impl_state_block.cpp %RS%\source\d3d11\d3d11_impl_type_convert.cpp %RS%\source\d3d11\d3d11_extensions.cpp
set OURSRC=reshade\core\rs_core.cpp reshade\core\rs_stubs.cpp reshade\core\rs_state_block.cpp reshade\core\rs_vswap.cpp reshade\core\rs_runtime.cpp reshade\core\rs_stage_io.cpp reshade\core\rs_stage_copy.cpp reshade\core\rs_game.cpp reshade\core\rs_api.cpp reshade\core\rs_assign.cpp

%CXX% %RSSRC% %OURSRC% || exit /b 1
%CXX% %RS%\deps\fpng\src\fpng.cpp || exit /b 1
%CXX% /TP %RS%\deps\jxl_simple_lossless\simple_lossless.cc || exit /b 1
cl /nologo /c /O2 /MT %DEFS% %INC% /Fo%OBJ%\ %RS%\deps\stb_impl.c || exit /b 1

link /nologo /dll /DEBUG /INCREMENTAL:NO /OPT:REF /PDB:%BIN%\s2fm_reshade_core.pdb /OUT:%BIN%\s2fm_reshade_core.dll %OBJ%\*.obj %OBJ%\rs_core.res d3d11.lib dxgi.lib d3dcompiler.lib dxguid.lib shlwapi.lib version.lib ole32.lib user32.lib advapi32.lib shell32.lib winmm.lib || exit /b 1
echo built %BIN%\s2fm_reshade_core.dll

rem The harness: a program of its own, beside the core.
cl /nologo /O2 /Zi /MT /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Ireshade\core /Ireshade /Fd%OBJ%\rs_harness_cl.pdb /Fo%OBJ%\harness\ /Fe%BIN%\rs_harness.exe reshade\harness\rs_harness.cpp d3d11.lib dxgi.lib d3dcompiler.lib user32.lib || exit /b 1
echo built %BIN%\rs_harness.exe

rem --- The tool's window.
set QTDEFS=/DUNICODE /DQT_DLL /DQT_NO_DEBUG /DQT_CORE_LIB /DQT_GUI_LIB
rem moc takes the same defines, but only in its own -D form: it reads /D as a file name.
set MOCDEFS=-DUNICODE -DQT_DLL -DQT_NO_DEBUG -DQT_CORE_LIB -DQT_GUI_LIB
rem /Ireshade for core\rs_core_api.h, which the core shares.
set QTINC=/Ireshade /I"%QTSRC%\include" /I"%QTSRC%\include\QtCore" /I"%QTSRC%\include\QtGui"
set TOBJ=%OBJ%\tool
set MOCED=rs_mainwindow
set TSOURCES=rs_tool rs_log rs_alloc rs_bridge rs_mainwindow
set TOBJS=
for %%h in (%MOCED%) do (
    "%QT4%\bin\moc.exe" %MOCDEFS% -Ireshade reshade\tool\%%h.h -o %TOBJ%\moc_%%h.cpp || exit /b 1
)
for %%s in (%TSOURCES%) do call :compile reshade\tool\%%s.cpp || exit /b 1
for %%h in (%MOCED%) do call :compile %TOBJ%\moc_%%h.cpp || exit /b 1
link /nologo /dll /DEBUG /INCREMENTAL:NO /PDB:%BIN%\s2fm_reshade.pdb /OUT:%BIN%\s2fm_reshade.dll %TOBJS% "%QT4%\lib\QtCore4.lib" "%QT4%\lib\QtGui4.lib" user32.lib kernel32.lib || exit /b 1
echo built %BIN%\s2fm_reshade.dll
endlocal
exit /b 0

:compile
cl /nologo /c /O2 /Zi /MT /EHsc /Zc:wchar_t- %QTDEFS% %QTINC% /Fo"%TOBJ%\%~n1.obj" /Fd"%TOBJ%\s2fm_reshade_cl.pdb" %1 || exit /b 1
set TOBJS=%TOBJS% "%TOBJ%\%~n1.obj"
exit /b 0
