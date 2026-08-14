@echo off
setlocal
rem Build AirZip.exe - static CRT (/MT) so the result has no runtime dependency.
rem
rem Deliberately does not use vswhere: it lives under "Program Files (x86)",
rem and cmd mis-parses those parentheses in nearly every context that would
rem expand the path. Probing the standard install roots is shorter and works.

cd /d "%~dp0"

set "PF86=%ProgramFiles(x86)%"
set "PF64=%ProgramFiles%"
set "VSPATH="

call :probe "%PF86%\Microsoft Visual Studio\2022"
call :probe "%PF64%\Microsoft Visual Studio\2022"
call :probe "%PF86%\Microsoft Visual Studio\2019"
call :probe "%PF64%\Microsoft Visual Studio\2019"

if not defined VSPATH (
  echo ERROR: no Visual Studio C++ toolset found.
  exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo ERROR: vcvars64.bat failed.
  exit /b 1
)

where cl.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: cl.exe not on PATH after vcvars.
  exit /b 1
)

where rc.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: rc.exe not on PATH - Windows SDK missing.
  exit /b 1
)

if not exist "..\7z.dll" (
  echo ERROR: ..\7z.dll missing - it is embedded into the exe.
  exit /b 1
)

if not exist out mkdir out

echo [1/2] resources
rc /nologo /fo out\airzip.res airzip.rc
if errorlevel 1 exit /b 1

echo [2/2] compile + link
cl /nologo /std:c++17 /EHsc /O1 /Os /GL /MT /DNDEBUG /W3 ^
   /Fo:out\ /Fe:out\AirZip.exe ^
   airzip.cpp out\airzip.res ^
   /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO
if errorlevel 1 exit /b 1

echo.
for %%f in (out\AirZip.exe) do echo Built: %%~ff  (%%~zf bytes)
exit /b 0

:probe
if exist "%~1\BuildTools\VC\Auxiliary\Build\vcvars64.bat"   set "VSPATH=%~1\BuildTools"
if exist "%~1\Community\VC\Auxiliary\Build\vcvars64.bat"    set "VSPATH=%~1\Community"
if exist "%~1\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%~1\Professional"
if exist "%~1\Enterprise\VC\Auxiliary\Build\vcvars64.bat"   set "VSPATH=%~1\Enterprise"
exit /b 0
