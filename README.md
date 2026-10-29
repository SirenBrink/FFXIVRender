# FFXIVRender

Very early, unfinished Windows x64 ReShade addon for Final Fantasy XIV. The build produces `XIVRender.addon64`.

## Pre-release

Includes native DLSS controls and game DLSS DRS, DLSS-G 2X–6X, XeFG 2X–4X, experimental FSR-FG 2X–4X through DLSS Enabler, ReShade depth compatibility, independent latency controls, dynamic MFG, Neural Rendering and multipass NR, NR controls and presets, and RenoDX-derived 10-bit PQ and 16-bit scRGB HDR. Availability depends on the GPU, driver and provider runtime.

**FSR-FG via DLSS Enabler is currently not working in-game.** Its implementation is included for development; passing isolated tests does not establish gameplay correctness. PreSR and REST HUD isolation remain pending. This is a source pre-release, not a ready-to-install package.

Assisted by GPT-6 Astra.

## Build

Requirements: Windows x64, Visual Studio C++ build tools and Windows SDK, CMake, Ninja, and a separately obtained NVIDIA NGX SDK. Python 3 and PowerShell are used by optional preparation and validation tools.

Redistributable dependency headers/source and their notices are in `external/`. The proprietary NGX headers, vendor runtime DLLs, model binaries, game files and local test artifacts are not included.

From a developer command prompt:

```bat
set NGX_SDK_DIR=C:\path\to\licensed\NGX\include
tools\build.cmd
```

The script configures CMake, builds the addon and test tools, and runs CTest. Alternatively:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNGX_SDK_DIR=C:\path\to\licensed\NGX\include
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

## Runtime preparation and validation

`tools/prepare-runtime.ps1`, `tools/prepare-fg-runtime.ps1` and `tools/prepare-sm86-runtime.ps1` describe the separately required runtime inputs. Check each script's parameters and the vendor's terms. Runtime and GPU-dependent probes require those local files; they are separate from CTest.

The experimental Enabler adapter can be prepared with `python tools/prepare-enabler-headless.py --source C:\path\to\version.dll`. It accepts only the exact supplied 4.9.0.15 binary and documents its local adaptation in a generated manifest. No proprietary binary is distributed here, and the path remains broken in-game.

`tools/install-first-test.ps1` is a development installer gated by exact-build local acceptance receipts. It is not a general release installer. Supply your game directory explicitly. Back up the installation before testing.

See [frame generation](docs/FRAME_GENERATION.md), [native NR](docs/NATIVE_NR.md), [HDR](docs/NATIVE_HDR.md), and [diagnostics](docs/FG_DIAGNOSTICS.md). Historical repair/validation notes describe earlier local tests, not guarantees about this pre-release; referenced logs, research captures and receipts are intentionally excluded.

## License

Application source: **GPL-3.0-or-later**, including components adapted from the SirenBrink OptiScaler fork. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Bundled dependencies retain their own licenses. No additional rights to proprietary SDKs, runtimes, models or game assets are granted.
