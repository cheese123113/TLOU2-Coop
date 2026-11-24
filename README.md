# winmm: client source

This folder holds the C++ source of the multiplayer client for The Last of Us Part II Remastered on PC.
It builds as `winmm.dll`. The game loads that file as its winmm proxy, and the same file also holds the mod code.

## How the shipped winmm.dll is made

Twelve Weeks does **not** build its `winmm.dll` from this folder. The release build:

1. starts from a fixed prebuilt binary (`tools/local/winmm-nocave-127.0.0.1-7001.dll`);
2. applies byte patches at fixed offsets (`tools/features/winmm_patches.py`, applied by `tools/features/build_dll.py`);
3. adds the `t2r-ext.dll` loader and the data caves, and sets the server address (`tools/features/patch_winmm.py`).

All these patches use fixed offsets. A `winmm.dll` rebuilt from this folder has different offsets, so the patches
do not fit it. Use this folder to read how the client works and to port changes. New features go in `tools/ext/`.

To install the prebuilt binary, see `tools/fetch_base_dll.py` and [CONTRIBUTING.md](../CONTRIBUTING.md).

## What is not here

- `psarc/`, `widgets/`, `music/`: data taken from the game (archive contents, UI widgets, menu music).
- Decompiler output and `local-config.cmake` (machine-local).

The archive keystream key, nonce and verify key in `CMakeLists.txt` are zero. Set your own with
`-DTO1_PSARC_KEY=... -DTO1_PSARC_NONCE=... -DTO1_VERIFY_KEY=...`.

## Building

Requirements:

- Windows, CMake 3.25 or later, Python 3.
- Visual Studio 2022 or 2026 with the "C++ Clang tools for Windows" component. Or an LLVM toolchain on PATH:
  `clang-cl`, `lld-link`, `llvm-rc`, `llvm-ml`, `llvm-lib`, `llvm-strip`.
- `gui2.exe` (widget baker) and `ndarc.exe` (archive packer) on PATH.
- Your own `psarc/` and `widgets/` folders next to `CMakeLists.txt`. The configure step stops when they are empty.

Steps:

1. Copy `local-config.cmake.example` to `local-config.cmake` and set your game folders.
2. Configure and build:

```bat
cmake --preset ninja-release
cmake --build --preset ninja-release
```

Or use the Visual Studio presets (`vs2022`, `to1-release`). `TO1Debug` adds logging and the NET debug menu (F1).
The build fetches safetyhook (with Zydis) from GitHub.
