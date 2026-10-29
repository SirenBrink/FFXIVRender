# Community references reviewed 2026-10-24

These are research inputs for later feature ports. No source from these projects or supplied foreign binary was incorporated into the current 0.1.3 addon.

## DLSS5 ReShade AIO

[Project](https://github.com/kibblerz/DLSS5-Reshade-AIO), [processing implementation](https://github.com/kibblerz/DLSS5-Reshade-AIO/blob/main/addon/src/nr-standalone.cpp), [third-party notices](https://github.com/kibblerz/DLSS5-Reshade-AIO/blob/main/THIRD_PARTY_NOTICES.md).

Its public implementation offers useful examples of D3D11/D3D12 shared textures and fences, fence-protected command slots, bounded queue pressure, real-frame fallback, GPU timings, clearing optional NR inputs and retaining the last successful multipass result after a later pass fails. Review these mechanisms when integrating NR and the owned FG presentation bridge.

It owns its own SR/NR/FG pipeline and presentation compositor. Its generic SR submits zero jitter and normally uses Optical Flow motion, while installation instructions disable native game upscaling/AA/FG. Those input contracts must not replace FFXIV's verified native guides or duplicate native SR history. The inspected source exposes experimental 2x FG rather than 6x/dMFG and does not establish full Reflex integration, NR reuse or FP8 support.

Original AIO code is Apache-2.0 with NOTICE/modified-file attribution requirements; Feeder and NVIDIA dependencies have their own terms. Pin the actual source revision and preserve required notices if code is adapted. This review followed main; source may change.

## Supplied RenoDX addon

`locally supplied renodx-dlss.addon64` was inspected as a PE file, without loading or installing it. Size: 3,333,632 bytes. File version: 0.2026.1016.1704. SHA256: `A8FD34B531FD22C3DBD008ED8FD3CBFCCA2FB451729E19CD1FC2E4C85B5068D4`. Exports: NAME, DESCRIPTION, RenoDXUpdateSetting.

Embedded strings indicate NGX/Streamline interception, Reflex markers, FG controls, NR resource capture and retirement mechanisms. These are static evidence of intended functionality, not verified runtime behavior. Its model-preset descriptions still refer to the next feature creation. Exact source matching this NR-controller binary was not established.

Public RenoDX MIT sources useful for later integration include [native NGX/device handling](https://github.com/clshortfuse/renodx/blob/main/src/utils/dlss/nvngx.hpp) and [Streamline proxy/tagging helpers](https://github.com/clshortfuse/renodx/blob/main/src/utils/dlss/streamline_v2.hpp). Public DLSSFix is a separate addon and should not be treated as the supplied binary's source.

## FFXIV frontend state

[FFXIVClientStructs GameMain](https://github.com/aers/FFXIVClientStructs/blob/main/FFXIVClientStructs/FFXIV/Client/Game/GameMain.cs) supplied the initial layout and signature for frontend detection. The direct singleton, 32-bit territory load state and connected/territory fields were checked against the installed executable before use. The native GraphicsConfig scaling gate and its other renderer readers were checked in Ghidra; the field is not named by ClientStructs. Research output is retained under diagnostics/.
