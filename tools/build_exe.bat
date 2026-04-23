@echo off
setlocal

:: ============================================================
::  tracex_recv.exe 빌드 스크립트
::  - PyInstaller 미설치 시 자동 설치
::  - 단일 파일(--onefile) exe 생성 → dist\tracex_recv.exe
:: ============================================================

cd /d "%~dp0"

echo [1/4] Python 확인 중...
python --version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python 을 찾을 수 없습니다. PATH 를 확인하세요.
    pause
    exit /b 1
)

echo [2/4] PyInstaller 설치 확인 중...
:: 시스템 경로 및 사용자 Scripts 경로 모두 탐색
set PYINSTALLER_EXE=
for /f "delims=" %%i in ('python -c "import os,sys; scripts=[os.path.join(p,'Scripts') for p in [sys.prefix, os.path.expandvars('%APPDATA%\\Python\\Python'+str(sys.version_info.major)+str(sys.version_info.minor))]] ; exe=[os.path.join(s,'pyinstaller.exe') for s in scripts if os.path.exists(os.path.join(s,'pyinstaller.exe'))]; print(exe[0] if exe else '')" 2^>nul') do set PYINSTALLER_EXE=%%i

if "%PYINSTALLER_EXE%"=="" (
    echo     PyInstaller 미설치 — pip 으로 설치합니다.
    python -m pip install pyinstaller
    if errorlevel 1 (
        echo [ERROR] pip 설치 실패. Python 및 pip 환경을 확인하세요.
        pause
        exit /b 1
    )
    for /f "delims=" %%i in ('python -c "import os,sys; scripts=[os.path.join(p,'Scripts') for p in [sys.prefix, os.path.expandvars('%APPDATA%\\Python\\Python'+str(sys.version_info.major)+str(sys.version_info.minor))]] ; exe=[os.path.join(s,'pyinstaller.exe') for s in scripts if os.path.exists(os.path.join(s,'pyinstaller.exe'))]; print(exe[0] if exe else '')" 2^>nul') do set PYINSTALLER_EXE=%%i
)
echo     pyinstaller: %PYINSTALLER_EXE%

echo [3/4] pyserial 설치 확인 중...
python -m pip show pyserial >nul 2>&1
if errorlevel 1 (
    echo     pyserial 미설치 — pip 으로 설치합니다.
    python -m pip install pyserial
)

echo [4/4] exe 빌드 중...
"%PYINSTALLER_EXE%" ^
    --onefile ^
    --console ^
    --name tracex_recv ^
    --distpath dist ^
    --workpath build_tmp ^
    --specpath build_tmp ^
    tracex_recv.py

if errorlevel 1 (
    echo.
    echo [ERROR] 빌드 실패. 위 오류 메시지를 확인하세요.
    pause
    exit /b 1
)

echo.
echo ============================================================
echo  빌드 완료: tools\dist\tracex_recv.exe
echo.
echo  사용법 (명령 프롬프트):
echo    tracex_recv.exe COM3 230400
echo    tracex_recv.exe COM3           (기본 baud: 230400)
echo.
echo  .trx 파일 저장 위치: exe 와 동일 폴더의 tracex_dumps\
echo ============================================================

pause
