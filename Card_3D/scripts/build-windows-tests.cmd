@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "CARD3D_VS=%%i"
if not defined CARD3D_VS exit /b 1
call "%CARD3D_VS%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\native mkdir build\native
cl /nologo /std:c++17 /O2 /EHsc /utf-8 /W4 /LD src\card_studio.cpp /Fo:build\native\card3d.obj /Fe:build\native\card3d.dll /link /IMPLIB:build\native\card3d.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /utf-8 /W4 /LD /DCARD3D_TEST_API src\card_studio.cpp /Fo:build\native\card3d_test.obj /Fe:build\native\card3d_test.dll /link /IMPLIB:build\native\card3d_test.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /utf-8 /W4 tests\sanitizer_smoke.cpp /Fo:build\native\memory_smoke.obj /Fe:build\native\memory_smoke.exe
if errorlevel 1 exit /b 1
if exist build\abi\frei0r.h (
  cl /nologo /std:c++17 /O2 /EHsc /utf-8 /W4 /Ibuild\abi tests\abi_host.cpp /Fo:build\native\abi_host.obj /Fe:build\native\abi_host.exe
  if errorlevel 1 exit /b 1
)
exit /b %errorlevel%
