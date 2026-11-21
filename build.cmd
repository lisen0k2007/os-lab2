@echo off 
setlocal 
cd /d "%%~dp0" 
 
call "C:\Program Files\Git\cmd\git.exe" pull --quiet --ff-only || exit /b
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug || exit /b  
cmake --build build || exit /b  
build\hello.exe
