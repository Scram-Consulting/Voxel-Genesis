@echo off
echo ============================================
echo   VoxelGenesis - Sandbox Infinito
echo ============================================
echo.

if not exist "build\bin\Release\VoxelWorld.exe" (
    echo ERROR: VoxelWorld.exe no encontrado
    echo.
    echo Compila primero con:
    echo   cmake -B build -G "Visual Studio 17 2022" -A x64
    echo   cmake --build build --config Release
    pause
    exit /b 1
)

echo Iniciando VoxelGenesis...
echo.
echo Controles:
echo   WASD          - Movimiento
echo   Mouse         - Rotar camara
echo   Espacio       - Saltar
echo   Clic izq/der  - Minar / colocar bloque
echo   1-9 / rueda   - Cambiar slot del hotbar
echo   E             - Inventario y crafteo
echo   ESC           - Menu de pausa
echo   F3            - Overlay de rendimiento
echo   F11           - Pantalla completa
echo.
echo Log: %LOCALAPPDATA%\VoxelGenesis\log.txt
echo.

cd build\bin\Release
start VoxelWorld.exe
