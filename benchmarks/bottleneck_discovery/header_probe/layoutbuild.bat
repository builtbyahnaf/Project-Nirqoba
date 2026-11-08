@echo off
call "C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
pushd "D:\Actual Project Files\Project Nirqoba"
cl /nologo /O2 /std:c++20 /EHsc /MD /W0 /Fe:"build\bottleneck_discovery\layout_probe\layout_candidates.exe" "benchmarks\bottleneck_discovery\header_probe\layout_candidates.cpp"
set rc=%errorlevel%
popd
exit /b %rc%