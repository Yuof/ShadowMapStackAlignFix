# ShadowMap Stack Align Fix

SKSE plugin for Skyrim Special Edition: SE 1.5.97 and AE 1.6.317 - 1.7.104, one DLL; tested on 1.6.1170. Fixes a
crash in [Engine Fixes](https://www.nexusmods.com/skyrimspecialedition/mods/17230) (7.0.19, 7.0.20 and the 7.0.21
betas carry the same code): an access violation in `EngineFixes.dll` (tbbmalloc's large-object cache, `movdqa` to the
stack).

Engine Fixes' `bBSLightingShaderPropertyShadowMap` fix jumps from the middle of the game's
`BSLightingShaderProperty::GetRenderPasses_ShadowMapOrMask` into a generated stub that calls its detour after
`push rbx; sub rsp, 0x20`. The push leaves the stack 8 bytes off the 16-byte alignment x64 code expects at a call, on
every call. Most code does not notice; tbbmalloc's cache cleanup, reached from the detour's allocation, stores SSE
registers to the stack with aligned moves and crashes. Reported upstream as
[aers/EngineFixesSkyrim64#102](https://github.com/aers/EngineFixesSkyrim64/issues/102).

The plugin follows the jump at the hook site to Engine Fixes' stub, checks it byte for byte, and points the jump at
its own stub: the same register setup and continuation, with the stack aligned before the call. Engine Fixes' DLL is
not changed. Anything else at that place (Engine Fixes absent, the fix turned off, an Engine Fixes release with a
corrected stub) leaves the plugin inactive, and `ShadowMapStackAlignFix.log` says so and lists the bytes it found.
While active it logs, at the 1st, 10th, 100th ... call, how many detour calls entered with a misaligned stack
(expected: 0).

## Requirements
- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- [Engine Fixes](https://www.nexusmods.com/skyrimspecialedition/mods/17230) with its ShadowMap fix on (the default; nothing to fix without it)

## Building
Visual Studio 2022 or newer (C++), CMake 3.24+, [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

```
git clone --recurse-submodules https://github.com/Yuof/ShadowMapStackAlignFix
cd ShadowMapStackAlignFix
cmake --preset release
cmake --build --preset release
```

The DLL is `build/Release/ShadowMapStackAlignFix.dll`. `-DREPRODUCE_MISALIGNMENT=ON` builds a probe whose stub keeps
Engine Fixes' 8-byte offset, to show the counter and the crash; never install it.

## Credits
- [williamrod98](https://github.com/williamrod98) found this bug, traced it to the stub and proposed the aligned
  instruction sequence in [#102](https://github.com/aers/EngineFixesSkyrim64/issues/102); this plugin's stub uses it.
- aers, Nukem and Ryan for Engine Fixes.

## License
[GPL-3.0-or-later](LICENSE), as required by [CommonLibSSE NG](https://github.com/alandtse/CommonLibVR) (GPL-3.0-or-later
with its modding exceptions), which this plugin links statically.
