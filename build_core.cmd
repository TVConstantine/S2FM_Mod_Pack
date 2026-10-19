@echo off
rem The package's dxgi.dll: the proxy core and the features on it (package\features.cpp), into bin\core\. It is the
rem system's dxgi.dll stand in, so it never links dxgi.lib. RTAO's compute shaders are built with the Windows SDK's
rem dxc into headers first.
setlocal
cd /d "%~dp0"
if not defined VCINSTALLDIR call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist bin\core mkdir bin\core
if not exist obj\core mkdir obj\core
dxc.exe -nologo -T cs_6_0 -E ToWorld -Vn kRtaoToWorld -Fh obj\core\rtao_toworld_cs.h rtao\rtao_cs.hlsl || exit /b 1
dxc.exe -nologo -T cs_6_5 -E AoMain -D AO_KERNEL=1 -Vn kRtaoAo -Fh obj\core\rtao_ao_cs.h rtao\rtao_cs.hlsl || exit /b 1
cl /nologo /c /O2 /Fo"obj\core\\" minhook\src\buffer.c minhook\src\hook.c minhook\src\trampoline.c minhook\src\hde\hde64.c || exit /b 1
set SOURCES=core\core_hooks.cpp core\core_crash.cpp core\core_views.cpp core\core_table.cpp core\core_log.cpp core\core_dev.cpp ^
    common\shader_table.cpp common\shader_table_build.cpp common\lzma_decode.cpp common\sfm_doc.cpp ^
    reshade\stages.cpp refraction\refract.cpp refraction\refract_blur.cpp ^
    rtao\rtao_engine.cpp rtao\rtao_capture.cpp rtao\rtao_d3d12.cpp package\features.cpp
cl /nologo /c /O2 /Zi /EHsc /std:c++17 /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Iobj\core /Fd"obj\core\core.pdb" /Fo"obj\core\\" %SOURCES% || exit /b 1
set OBJS=
for %%s in (%SOURCES%) do call set OBJS=%%OBJS%% obj\core\%%~ns.obj
link /nologo /dll /DEBUG /OPT:REF /OPT:ICF /DEF:core\dxgi.def /OUT:bin\core\dxgi.dll /PDB:bin\core\dxgi.pdb ^
    obj\core\buffer.obj obj\core\hook.obj obj\core\trampoline.obj obj\core\hde64.obj %OBJS% ^
    user32.lib kernel32.lib d3dcompiler.lib d3d12.lib dxguid.lib || exit /b 1
echo built bin\core\dxgi.dll
