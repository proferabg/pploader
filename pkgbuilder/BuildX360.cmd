@echo off
setlocal
set "X360_ROOT=%~dp0X360"
set "X360_PATCH=%~dp0patches\X360-pkgbuilder.patch"
set "X360_SOURCE=X360/X360/STFS/STFSPackage.cs"
set "X360_CONFIG=%~1"
if not defined X360_CONFIG set "X360_CONFIG=Release"

if not exist "%X360_ROOT%\.git" (
    echo ERROR: X360 submodule is not initialized.
    echo Run: git submodule update --init --recursive
    exit /b 1
)

git -C "%X360_ROOT%" diff --quiet -- "%X360_SOURCE%"
if errorlevel 1 (
    echo ERROR: X360 source has local changes; refusing to overwrite them.
    exit /b 1
)

git -C "%X360_ROOT%" apply --check "%X360_PATCH%"
if errorlevel 1 (
    echo ERROR: X360 package-builder patch does not apply to the pinned revision.
    exit /b 1
)

git -C "%X360_ROOT%" apply "%X360_PATCH%"
if errorlevel 1 exit /b 1

"%SystemRoot%\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe" "%X360_ROOT%\X360\X360\X360.csproj" /t:Rebuild /p:Configuration=%X360_CONFIG% /p:Platform=AnyCPU /v:minimal
set "X360_RESULT=%ERRORLEVEL%"

git -C "%X360_ROOT%" checkout -- "%X360_SOURCE%"
if errorlevel 1 exit /b 1

exit /b %X360_RESULT%
