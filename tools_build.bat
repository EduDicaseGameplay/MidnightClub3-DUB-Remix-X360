@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
cmake -S x360/tools -B x360/tools/build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build x360/tools/build || exit /b 1
