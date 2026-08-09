# Archivo histórico

Estos 69 documentos son el registro de las sesiones de desarrollo anteriores a
que el proyecto tuviera control de versiones (mayo–julio 2025). Se conservan por
su valor histórico, **no como documentación de referencia**.

## Por qué no fiarse de ellos

La auditoría de 2026-08-08 (`docs/AUDITORIA-INGENIERIA-2026-08-08.md`) verificó
sus afirmaciones contra el código y encontró que se contradicen entre sí y con la
implementación real. Ejemplos concretos:

- `OPTIMIZACION_120_FPS.md` afirma que el VSync está desactivado en la "línea
  3422"; el código real tenía `glfwSwapInterval(1)` en otra línea. Los documentos
  `SISTEMA_FINAL_60FPS_GARANTIZADOS.md`, `URGENT_FPS_FIX.md` e
  `IMMEDIATE_ACTION.md` afirman lo contrario que el de 120 FPS.
- `FINAL_SUMMARY.md` y `60FPS_READY.md` dan por entregado un overlay de profiler
  con tecla F3 que no existía en el código (`ANALYSIS_REPORT.md`, el único
  documento veraz del conjunto, ya lo desmentía). El overlay F3 se implementó de
  verdad en 2026-08-09.
- `60FPS_READY.md` lista como integradas cinco cabeceras (`GreedyMesher.h`,
  `AdaptiveQuality.h`, `PerformanceGuarantee.h`, `ObjectPool.h`,
  `RenderOptimizations.h`) que tenían **cero referencias** en el código. Todas se
  eliminaron en la limpieza de 2026-08-09.
- El README original del proyecto (conservado aquí como
  `README-original-2025.md`) prometía "8 tipos de bloques" (el enum tiene más de
  30), situaba los mundos en `projects/` (están en `saves/`) y describía un motor
  de "display lists" cuando el código exige extensiones VBO.

Hay al menos 10 documentos sobre "arreglar los FPS" y 6 "resúmenes finales", todos
escritos como si fueran la versión definitiva.

## Dónde está la información actual

- **`README.md`** en la raíz del repositorio: cómo compilar, ejecutar y qué hay
  en cada carpeta.
- **`docs/AUDITORIA-INGENIERIA-2026-08-08.md`**: estado real del proyecto,
  hallazgos verificados con referencias a archivo y línea, y el plan de trabajo.
- **El historial de git**: desde 2026-08-08 cada cambio queda registrado con su
  razón. `git log` es más fiable que cualquier documento de esta carpeta.

También se archiva aquí `modern_voxel_engine.cpp`, un fragmento de código suelto
de 346 líneas que nunca formó parte del build.
