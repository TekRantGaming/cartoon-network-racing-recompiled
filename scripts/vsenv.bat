@echo off
rem Usage: vsenv.bat <command...>  -- runs command inside the VS 2022 Build Tools x64 environment
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
%*
