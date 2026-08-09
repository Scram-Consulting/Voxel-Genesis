@echo off
REM ============================================================================
REM VoxelGenesis - Creador de version portable
REM Ejecutar desde la raiz del proyecto:  tools\create_portable.bat
REM ============================================================================

echo ============================================
echo   VoxelGenesis - Version Portable
echo ============================================
echo.

set PORTABLE_DIR=VoxelWorld_Portable
set BUILD_DIR=build\bin\Release

if not exist "%BUILD_DIR%\VoxelWorld.exe" (
    echo ERROR: VoxelWorld.exe no encontrado
    echo Compila primero con: cmake --build build --config Release
    pause
    exit /b 1
)

if not exist "resourcepacks" (
    echo ERROR: ejecuta este script desde la raiz del proyecto
    pause
    exit /b 1
)

if exist "%PORTABLE_DIR%" (
    echo Eliminando version portable anterior...
    rmdir /s /q "%PORTABLE_DIR%"
)

echo Creando estructura de carpetas...
mkdir "%PORTABLE_DIR%"

echo Copiando ejecutable...
copy "%BUILD_DIR%\VoxelWorld.exe" "%PORTABLE_DIR%\" >nul

REM El juego busca resourcepacks/ y sounds/ subiendo desde la carpeta del
REM ejecutable: sin copiarlos arranca sin texturas ni sonido.
echo Copiando texturas...
xcopy /E /I /Q "resourcepacks" "%PORTABLE_DIR%\resourcepacks" >nul

echo Copiando sonidos...
xcopy /E /I /Q "sounds" "%PORTABLE_DIR%\sounds" >nul

echo Creando README.txt...
(
echo ============================================
echo   VoxelGenesis - Sandbox Infinito
echo ============================================
echo.
echo Para jugar: haz doble clic en VoxelWorld.exe
echo Tus mundos se guardan en: saves\
echo.
echo Controles:
echo   WASD          - Movimiento
echo   Mouse         - Rotar camara
echo   Espacio       - Saltar
echo   Clic izq      - Minar bloque
echo   Clic der      - Colocar bloque
echo   1-9 / rueda   - Cambiar slot del hotbar
echo   E             - Inventario y crafteo
echo   ESC           - Menu de pausa
echo   F3            - Overlay de rendimiento
echo   F11           - Pantalla completa
echo.
echo Si algo falla, revisa el log en:
echo   %%LOCALAPPDATA%%\VoxelGenesis\log.txt
echo.
echo Generado: %date% %time%
) > "%PORTABLE_DIR%\README.txt"

echo Creando acceso directo...
powershell -Command "$ws = New-Object -ComObject WScript.Shell; $s = $ws.CreateShortcut('%CD%\%PORTABLE_DIR%\VoxelWorld.lnk'); $s.TargetPath = '%CD%\%PORTABLE_DIR%\VoxelWorld.exe'; $s.WorkingDirectory = '%CD%\%PORTABLE_DIR%'; $s.Description = 'VoxelGenesis - Sandbox Infinito'; $s.Save()"

echo.
echo ============================================
echo   VERSION PORTABLE CREADA
echo ============================================
echo.
echo Ubicacion: %CD%\%PORTABLE_DIR%\
echo.
pause
