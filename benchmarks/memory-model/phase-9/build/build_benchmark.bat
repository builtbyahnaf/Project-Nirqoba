@echo off
call "C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /MD /EHsc /I"D:\Actual Project Files\Project Nirqoba\runtime\include" "D:\Actual Project Files\Project Nirqoba\benchmarks\memory-model\phase-9\main.cpp" /Fe:"D:\Actual Project Files\Project Nirqoba\benchmarks\memory-model\phase-9\build\phase9_benchmark.exe" /link "D:\Actual Project Files\Project Nirqoba\build\msvc-dev\lib\nirqoba_runtime.lib" psapi.lib
exit /b %errorlevel%
