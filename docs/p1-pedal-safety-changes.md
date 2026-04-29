# P1 Pedal Safety & Stability Changes

Fecha: 2026-04-28

## Alcance

P1 se ejecutó después de confirmar que el P0 de factories JUCE IIR en `CleanAmp` y cabinets quedó documentado en `docs/p0-iir-coefficients-cleanamp-cabinets.md`.

No se tocó UI, wizards, IDs de parámetros, preset schema ni arquitectura de `AudioEngine`.

Nota de paths: los cabinets existen en `Source/Effects/Cabinets/`, no en `Source/Effects/Pedals/Cabinet/`.

## Archivos modificados en esta fase

- `Source/Effects/Cabinets/CabinetPedal.h`
- `Source/Effects/Cabinets/Vintage2x12Cabinet.h`
- `Source/Effects/Cabinets/Modern4x12Cabinet.h`
- `Source/Effects/Pedals/Compressor/CompressorPedal.h`
- `Source/Effects/Pedals/Distortion/DistortionPedal.h`
- `Source/Effects/Pedals/Fuzz/FuzzPedal.h`
- `Source/Effects/Pedals/Neural/NeuralPedal.h`
- `Source/Effects/Pedals/Wah/ClassicWahPedal.h`
- `Source/Effects/Pedals/Phaser/PhaserPedal.h`
- `Source/Core/AudioEngineTests.cpp`
- `docs/p1-pedal-safety-changes.md`

## Problemas corregidos

- Cabinets: `scratchBuffer.setSize()` ya no ocurre desde `processBlock`; buffers y spec se preparan con `preparedBlockSize`/`preparedChannels`.
- Cabinets: se agregó fallback dry/pass-through sin allocation si el host entrega más samples/canales que lo preparado.
- Cabinets: se agregó cleanup DC post-cab a 18 Hz en la ruta wet.
- Compressor: `dryBuffer.setSize()` ya no ocurre desde `processBlock`; fallback dry/pass-through y contador diagnóstico.
- Distortion/Fuzz: `scratchBuffer.setSize()` ya no ocurre desde `processBlock`; fallback dry/pass-through, sanitización NaN/Inf y contador diagnóstico.
- Neural: los bancos helper ya no redimensionan canales desde `processBlock`; se preasignan en `prepareToPlay`, se valida capacidad en proceso y se usa fallback dry si se excede.
- Wah: `scratchBuffer.setSize()` ya no ocurre desde `processBlock`; se agregó guard DC wet a 18 Hz y fallback dry/pass-through.
- Phaser: se reprodujo el fallo de DC accumulation; se corrigió con cleanup DC wet a 18 Hz y el test histórico deja de fallar.
- Tests: se agregó `P1 Pedal Safety` con matriz de sample rates, block sizes, señales, fallback oversized y bypass continuity para los 9 pedales P1.

## Fallbacks no-allocation

- `CabinetPedal`, `Vintage2x12Cabinet`, `Modern4x12Cabinet`: dry/pass-through, incrementa `fallbackBlockCount`.
- `CompressorPedal`: dry/pass-through, incrementa `fallbackBlockCount` y resetea gain reduction visible a 0 dB.
- `DistortionPedal`, `FuzzPedal`: dry/pass-through, incrementa `fallbackBlockCount`.
- `NeuralPedal`: dry/pass-through si el bloque/canales exceden capacidad preparada; también protege helpers internos preasignados.
- `ClassicWahPedal`: dry/pass-through, incrementa `fallbackBlockCount`.
- `PhaserPedal`: dry/pass-through, incrementa `fallbackBlockCount`.

Todos los fallbacks evitan `setSize`, `resize` y allocations condicionales en el audio thread.

## Cambios con posible impacto tonal

- No hubo revoicing intencional.
- Se agregaron filtros DC subsonic de 18 Hz en cabinets, wah wet y phaser wet. El objetivo es remover offset/estado acumulado, no modificar el carácter audible.
- Los fallbacks solo cambian comportamiento bajo condiciones anómalas de host: bloque/canales mayores que lo preparado. En condiciones normales la ruta tonal queda igual.

## Verificación estática

Comandos/búsquedas usados:

- `rg -n "setSize\(|resize\(|ensureChannels|IIR::Coefficients<float>::make" ...targets P1...`
- `Select-String ... -Pattern "IIR::Coefficients<float>::make|ensureChannels|\.resize\("`
- `Select-String ... -Pattern "void processBlock|scratchBuffer\.setSize|dryBuffer\.setSize|setSize\(|fallbackBlockCount|outputDcBlock|postCabDcBlock"`

Resultado:

- No quedan `IIR::Coefficients<float>::make*` en los targets P1.
- No quedan `ensureChannels` ni `.resize(` en los targets P1.
- Las únicas coincidencias `scratchBuffer.setSize`/`dryBuffer.setSize` en targets P1 están en `prepareToPlay`, antes de `processBlock`.
- `NeuralPedal` conserva `std::vector<float>` como storage interno, pero se asigna con `assign()` en `prepareToPlay`; no hay resize en `processBlock`.

## Pruebas ejecutadas

- `powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_SharedCode`
  - Resultado: PASS, warnings preexistentes de `ProcessorBase`/`AudioEngine`.
- `powershell -ExecutionPolicy Bypass -File scripts\build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_StandalonePlugin`
  - Resultado: PASS.
- `powershell -ExecutionPolicy Bypass -File scripts\run-base-audio-validation.ps1 -Configuration Debug -Platform x64 -TimeoutSeconds 180`
  - Resultado: FAIL global del script por tests no-P1.
  - Reporte final: `results=136 passes=5712 failures=46 failingResults=12`.
  - Ya no aparece el fallo `PhaserPedal feedback loop rejects DC accumulation under sustained bias`.
  - No aparecen fallos de `P1 Pedal Safety`.

## Fallos restantes fuera de P1

El reporte actual sigue listando fallos en:

- `InputChain preserves single-jack guitar level when input arrives on one channel`
- `OutputChain protects limiter headroom from biased input`
- `OutputChain clamps legacy extreme limiter thresholds to an audible floor`
- `Global processors preserve active params after reset`
- varios tests de `AudioEngine` clean/dry/latency
- `ReverbPedal produces a long finite tail that decays cleanly`
- `ReverbPedal swell softens the wet attack and blooms afterward`
- `ReverbPedal reverse and swell create a delayed cinematic bloom`

Estos fallos no se corrigieron en esta fase porque el pedido excluía refactor de `AudioEngine` y rebuild de otros pedales.

## Pedales P1 considerados RT-safe para esta superficie

- Cabinet
- Vintage 2x12
- Modern 4x12
- Compressor
- Distortion
- Fuzz
- Neural
- Wah
- Phaser

Criterio aplicado: sin `setSize`/`resize` en `processBlock`, helpers de canales preasignados, fallback no-allocation y salida finita bajo matriz P1.

## Requieren revisión humana adicional

- `ReverbPedal`: tests de tail/swell/reverse siguen fallando.
- `InputChain`, `OutputChain`, `AudioEngine`: fallos globales activos en la validación base.
- Full RT profiling: todavía falta instrumentación externa para confirmar ausencia de allocations en runtime más allá de la revisión estática.
- Tonal QA: los filtros DC nuevos son subsonic, pero conviene hacer null/sweep audit de cabinets, wah y phaser contra baseline antes de release.

## Recomendaciones P2

- Separar la suite de validación en grupos para que P1/P2 puedan fallar de forma aislada sin bloquearse por tests globales históricos.
- Corregir los fallos globales de `InputChain`, `OutputChain` y `AudioEngine` antes de endurecer rutas más profundas.
- Hacer audit RT de Reverb/Delay/modulation restantes con el mismo patrón de fallback counters.
- Agregar golden renders o tolerancias espectrales para validar que DC cleanup no se confunda con revoicing.
- Añadir test runner que exporte también tests pasados por grupo, no solo fallos.
