# Third-party notices

XIVRender application source is GPL-3.0-or-later. See LICENSE.

DLSS Enabler 4.9.0.15: supplied locally by the user, authored by Artur Graniszewski. The private runtime is a locally adapted copy of the supplied proxy DLL, not an official headless distribution. Only its proxy callback is bypassed; the original input and exact patch/hash manifest are retained. No redistribution rights for this proprietary component are implied. Source project: https://github.com/artur-graniszewski/DLSS-Enabler .

The normalized depth and automatic mip binder adapt components from the SirenBrink OptiScaler fork. Those files retain GPL attribution. The native NR caller boundary follows that fork's verified direct-snippet route. The native adapter carries the verified FFXIV behavior forward in separate modules rather than linking the OptiScaler runtime.

- ReShade API headers: Patrick Mours, BSD-3-Clause OR MIT; license under external/reshade/LICENSE.md.
- Dear ImGui 1.92.5 docking headers: Omar Cornut and contributors, MIT; external/imgui/LICENSE.txt. The addon uses ReShade's matching forwarding table, not a separately linked ImGui implementation.
- MinHook 1.3.4: Tsuda Kageyu and contributors; external/minhook/LICENSE.txt includes the bundled decoder notices.
- Streamline SDK public headers: NVIDIA copyright and MIT terms included in each header. Production DLLs and proprietary model/header assets retain NVIDIA's separate SDK/model terms. The source package does not grant additional NVIDIA redistribution rights.
- NVIDIA NGX SDK headers: supplied separately through NGX_SDK_DIR under the user's SDK license; not included in the source archive.

The mock NGX DLL is original test code, not an NVIDIA model. It is excluded from deployment packages.

- The optional SM75/SM86 bootstrap follows the GPL SirenBrink OptiScaler fork's AmpereMfgLoader initialization and control ABI. `tools/prepare-sm86-runtime.ps1` extracts the user's SilyNoMeta `dlssg_for_sm86` v0.3.5-5 package into a private runtime directory. Upstream: https://github.com/SilyNoMeta/dlssg_for_sm86 (fork of https://github.com/sdli1995/dlssg_for_sm86). The original package README and SHA256SUMS are retained beside the module; embedded NVIDIA components remain subject to their own terms. The application license does not grant redistribution rights for those components.

- RenoDX FFXIV scene shaders: Ersh, Pumbo, Carlos Lopez Jr. (ShortFuse) and contributors, MIT. Embedded shader provenance and license are under include/xivrender/renodx/. The HDR scene replay and UI transmittance helpers adapt the GPL SirenBrink OptiScaler fork; no OptiScaler runtime is linked. The pinned scene shaders are used as an independent branch, rather than running the official RenoDX addon alongside another HDR controller.

- Intel XeSS SDK 3.0.2: Intel Corporation, license under external/xess/LICENSE.txt and runtime/fg/xess.LICENSE.txt. The exact-build in-memory XeFG 4X compatibility patch and pacing scheduler adapt the GPL SirenBrink OptiScaler fork. Provider files on disk are unmodified.
- AMD FidelityFX SDK 2.3.0 frame generation: AMD, license and third-party notices under external/fsr/ and runtime/fg/. Anti-Lag 2 SDK 2.0.0a: AMD, external/antilag2/LICENSE.txt and runtime/fg/antilag2.LICENSE.txt. Driver capability determines latency feature availability.
# DLSS Enabler (optional FSR MFG runtime)

The experimental FSR multi-frame integration targets DLSS Enabler 4.9.0 STABLE by artur07305, file version 4.9.0.15. `tools/prepare-enabler-headless.py` derives a local private API adapter and records both hashes and the exact patch in `runtime/fg/enabler-manifest.json`. It is not an official headless distribution and is currently not working in-game. Neither the original nor the adapted binary is included in this source repository. Author page: https://www.nexusmods.com/site/mods/757.
