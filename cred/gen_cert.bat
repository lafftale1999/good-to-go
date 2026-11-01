@echo off
rem Windows (cmd.exe) entry point. The work is done by gen_cert.py so the result
rem is the same on every OS. See gen_cert.py for the outputs.
rem
rem Usage: gen_cert.bat [--force-ca]

setlocal

where python >nul 2>&1
if not errorlevel 1 goto run_python
where py >nul 2>&1
if not errorlevel 1 goto run_py

echo error: Python 3 not found. Run this from an ESP-IDF terminal (export.bat). 1>&2
exit /b 1

:run_python
python "%~dp0gen_cert.py" %*
exit /b %ERRORLEVEL%

:run_py
py -3 "%~dp0gen_cert.py" %*
exit /b %ERRORLEVEL%
