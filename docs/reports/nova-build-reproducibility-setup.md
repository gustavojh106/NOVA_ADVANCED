# NOVA Build Reproducibility Setup

## Current Risk

NOVA currently depends on a local JUCE 8.0.12 checkout exported through Projucer. `NOVA.jucer` stores module paths as:

```text
../../../../Downloads/juce-8.0.12-windows/JUCE/modules
```

From this repo location, that resolves to:

```text
C:\Users\gusta\Downloads\juce-8.0.12-windows\JUCE\modules
```

Generated Visual Studio project files also contain exported JUCE paths under that same local checkout.

## Required Local Setup

Until JUCE is vendored or the Projucer export is moved to an environment-driven module path, this machine setup is required:

1. Install Visual Studio 2022 with MSBuild v143 and a Windows 10 SDK.
2. Place JUCE 8.0.12 at:

```text
C:\Users\gusta\Downloads\juce-8.0.12-windows\JUCE
```

3. Confirm these directories exist:

```text
C:\Users\gusta\Downloads\juce-8.0.12-windows\JUCE\modules
C:\Users\gusta\Downloads\juce-8.0.12-windows\JUCE\modules\juce_audio_processors_headless\format_types\VST3_SDK
```

4. Build through the repo scripts, not by invoking generated project files directly:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Debug -Platform x64 -Target NOVA_StandalonePlugin
powershell -ExecutionPolicy Bypass -File scripts/build-nova.ps1 -Configuration Release -Platform x64 -Target NOVA_VST3
```

Release builds intentionally run MSBuild `Rebuild` through this script, even without `-Clean`. This keeps production Release artifacts from reusing stale SharedCode outputs or explicit Release-diagnostic objects.

## Deferred Fix

The safer long-term fix is one of:

- vendor JUCE modules into the repo and update `NOVA.jucer` to use local copies, or
- standardize an environment-driven module path and re-export the Visual Studio projects from Projucer.

That should be done in a dedicated build-system pass because generated Visual Studio projects are not the source of truth and a bad Projucer export can break the currently working build.
