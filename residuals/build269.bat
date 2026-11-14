@echo off
call "C:/Program Files/Microsoft Visual Studio/18/BuildTools/VC/Auxiliary/Build/vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d "D:/Actual Project Files/Project Nirqoba"
cmake --build build/msvc-dev --target nirqoba nirqoba_unit_tests -j 12
