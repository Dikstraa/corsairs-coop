# Corsairs Ship Pack — Co-op mod

Co-op multiplayer mod for **MS Corsairs Ship Pack** (Maelstrom Engine x64).

The repository holds only the mod and the game scripts. Game binaries and `RESOURCE/`
come from an installed copy of the Ship Pack: clone this repo over the game folder.

## Layout

- `CoopMod/src/` — native module `CoopNet.dll` (script library `COOPNET`).
  - `maelstrom_abi.h` — engine interfaces with vtable slots verified against the stock `modules/*.dll`.
- `CoopMod/build.bat` — builds the DLL (VS 2022 Build Tools, x64) and copies it to `modules/`.
- `Program/` — game scripts. The first commit is the unmodified Ship Pack; later commits are mod changes.
- `Program/coop/` — script side of the mod.

## Requirements

- DirectX 9 runtime from the Microsoft DirectX End-User Runtime (June 2010), x64:
  - `d3dx9_43.dll` — without it `Animation.dll`, `Sea.dll`, `Weather.dll` and `renderer/dx9.dll`
    fail with error 126 and the game exits with "No service: RENDER".
  - `D3DCompiler_43.dll` — loaded dynamically by d3dx9 to compile shaders. Without it `dx9.dll`
    crashes on a null pointer while compiling techniques and the game silently closes right after
    "Starting DirectX 9".

  Either install the full runtime (`directx_Jun2010_redist.exe` → `DXSETUP.exe`) or extract both
  x64 DLLs from its `Jun2010_d3dx9_43_x64.cab` / `Jun2010_D3DCompiler_43_x64.cab` next to `Maelstrom.exe`.

## Encoding

Scripts are Windows-1251 with CRLF line endings. `.gitattributes` disables any conversion;
edit them only with tools that preserve the encoding.
