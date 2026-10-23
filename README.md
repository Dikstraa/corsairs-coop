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

## Encoding

Scripts are Windows-1251 with CRLF line endings. `.gitattributes` disables any conversion;
edit them only with tools that preserve the encoding.
