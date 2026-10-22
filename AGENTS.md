# AGENTS.md: SurfacingPro

Start here before editing. Written for AI agents; useful to people too.

## What it is

An Unreal Engine 5.8 editor mode: model with primitives and bezier patches in the level, surface them by projection with layered PBR materials, ship as merged Nanite static meshes without a bake step.

## Layout

- `SurfacingPro/Source/SurfacingPro`: runtime module (the built actors)
- `SurfacingPro/Source/SurfacingProEditor`: the editor mode, windows and tools
- `SurfacingPro/Source/SurfacingProEditor/Private/Tests`: automation tests (`SurfacingPro.*`)
- `SurfacingPro/Docs/UserGuide.md`: user-facing behavior
- `docs/SPECS.md`: versions and requirements

## Build and test

C++ project, Visual Studio 2022, Unreal Engine 5.8. Tests: Session Frontend → Automation → `SurfacingPro`, or `UnrealEditor-Cmd <Project>.uproject -ExecCmds="Automation RunTests SurfacingPro; Quit" -unattended -nullrhi`.

## Rules

- Keep copyright headers and license notices.
- Use standard level-design and surfacing words (brush, patch, displacement, hotspot, trim sheet, lightmap, carve, blockout) rather than invented ones.
- Never copy code, UI layouts, icons, artwork or documentation text from other tools.
- License: `LICENSE.md`.
