# VoxelGenesis

Motor de vóxeles tipo Minecraft en C++20 y OpenGL: mundo infinito con generación
procedural por chunks, minado y colocación de bloques, inventario con crafteo 3x3
y guardado persistente de mundos.

**Plataforma:** solo Windows (usa Win32 para audio y diálogos).

## Compilar

Requiere CMake ≥ 3.20 y un compilador con soporte de C++20 (Visual Studio 2019 o
2022 con el workload *Desktop development with C++*). GLFW y stb_image vienen
vendorizados en `external/`, así que no hay que instalar dependencias.

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Con Visual Studio 2019, usa `-G "Visual Studio 16 2019"`.

El ejecutable queda en `build/bin/Release/VoxelWorld.exe`.

## Ejecutar

```bash
run.bat
```

O directamente el `.exe`. El juego localiza `resourcepacks/` y `sounds/` subiendo
desde la carpeta del ejecutable, así que funciona desde cualquier ubicación
mientras el árbol del proyecto esté intacto.

### Controles

| Tecla | Acción |
|---|---|
| `WASD` | Moverse |
| Ratón | Mirar |
| `Espacio` | Saltar |
| Clic izquierdo | Minar bloque |
| Clic derecho | Colocar bloque |
| `1`–`9` | Seleccionar slot del hotbar |
| Rueda del ratón | Cambiar de slot |
| `E` | Abrir/cerrar inventario y crafteo |
| `ESC` | Menú de pausa |
| `F3` | Overlay de rendimiento (FPS, chunks, memoria) |
| `F11` | Pantalla completa |

## Tests

```bash
ctest --test-dir build -C Release --output-on-failure
```

54 casos con [doctest](https://github.com/doctest/doctest) (vendorizado en
`external/doctest`) sobre la lógica que protege los datos del jugador:
compresión y serialización de chunks, almacenamiento paletizado, inventario y
validación de nombres de mundo. Se compilan por defecto; usa
`-DVOXEL_BUILD_TESTS=OFF` para omitirlos.

## Estructura

```
src/                Código fuente
  main.cpp          Motor y juego (~16k líneas; se está desmontando por partes)
  ChunkSystem.*     Gestión de chunks con hilos (parcialmente integrado)
  SaveSystem.*      Persistencia: archivos de región .vxr, compresión, backups
  Profiler.*        Overlay de rendimiento (F3)
  BlockType.h       Enum de bloques
  Inventory.h       Inventario y slots
  WorldName.h       Validación de nombres de mundo
  PalettedStorage.h Almacenamiento de bloques comprimido por paleta
tests/              Tests unitarios (doctest)
external/           GLFW, stb_image, doctest
resourcepacks/      Texturas
sounds/             Efectos de sonido
saves/              Mundos guardados (no versionado)
docs/               Auditoría de ingeniería y archivo histórico
tools/              Utilidades y scripts de parcheo antiguos (congelados)
```

## Dónde miran los logs

El binario se enlaza sin consola, así que la salida va a un archivo:

```
%LOCALAPPDATA%\VoxelGenesis\log.txt
```

Los errores fatales además muestran un diálogo. Si el juego se cierra de forma
inesperada, la siguiente ejecución lo detecta y avisa.

## Estado del proyecto

`docs/AUDITORIA-INGENIERIA-2026-08-08.md` contiene una auditoría completa con el
estado real, hallazgos verificados y el plan de trabajo. Resumen de lo que ya se
abordó y lo que queda:

- ✅ Control de versiones, build reproducible, logging visible
- ✅ Integridad de datos: corregidos varios bugs que corrompían mundos en
  silencio (formato de guardado en v2, con lectura de saves v1)
- ✅ Suite de tests sobre la lógica crítica
- ✅ Eliminadas ~7000 líneas de código muerto
- ⬜ Rendimiento: falta mover generación, meshing e iluminación a hilos de
  trabajo (la infraestructura existe pero nunca se arranca) e integrar greedy
  meshing. La iluminación está desactivada desde un intento previo de subir FPS.
- ⬜ Arquitectura: `main.cpp` sigue siendo un monolito con una clase `World` que
  concentra demasiadas responsabilidades

## Notas para desarrollar

Los ~45 scripts de Python en `tools/legacy-patches/` son el mecanismo de edición
que se usó antes de tener git: modifican `src/main.cpp` por búsqueda y reemplazo
de texto. **No los ejecutes**; están congelados solo como referencia histórica y
corrompen el archivo si se aplican sobre código ya parcheado.

Los documentos de `docs/archive/` son registros de sesiones antiguas y se
contradicen entre sí y con el código. No los uses como referencia; ver
`docs/archive/README.md`.
