# Migration matrix

Status distinguishes implemented code, controlled tests and confirmed gameplay. Native DLSS quality/model switching and frontend scaling are confirmed in-game. The user confirmed 0.2.4 NR controls and two-pass rendering. Scene HDR through 0.3.4 was confirmed in-game. Version 0.4.0 adds FG; provider/format/resize tests pass through actual ReShade/REST, while gameplay pacing and combined NR/FG behavior await confirmation.

| Feature | Current state | Acceptance before enabling in a replacement release |
|---|---|---|
| FFXIV native DLSS | Confirmed in-game, including login and character selection | Continue transition coverage as NR/HDR/FG are integrated |
| Live quality | Confirmed native scene subrect changes in-game | Continue rapid-switch coverage with optional processing enabled |
| Live DLSS model | Confirmed model-only native recreation in-game | Model availability remains runtime-dependent; continue combined-feature checks |
| Native jitter | Camera generator and queued NGX packet hooks implemented | Quality/Balanced/Performance and all model presets, stationary/moving, HDR/NR later; no post-raster synthetic phase |
| Texture bias | Immutable sampler variants and logical Get semantics implemented; WARP tests | Live quality changes and ReShade menu/effect interaction |
| ReShade depth | Active-region capture and temporary binding implemented; WARP tests | DisplayDepth/effects across quality, window resizing, loading and user manual overrides |
| Full Streamline | Private D3D12 SDK/proxy presentation integrated; actual generated counts verified | Explicit initialized private DX12 owner, exact native/proxy interfaces, coherent libraries and real rendered/presented telemetry |
| Reflex | Native simulation/render/present markers and SDK sleep integrated; controlled presentation passed | Real simulation/render/present markers, correct frame identity, native cap preservation, no repeated option updates |
| DLSS-G | Private presentation bridge integrated; SDR/PQ and 4K resize/recovery passed | Cold startup, loading, off/on, focus changes, refresh/VSync/VRR changes, memory pressure, SDR/HDR |
| DLSS-G presets | A/B/C request types implemented; feature transition pending | Selected runtime identified; controlled fence-safe recreation without restart and actual activation confirmation |
| DLSS-G MFG / dynamic | 2X to 6X actual SDK counts verified; sustained dynamic-factor policy implemented | 2X through 6X, live factor swaps, target changes and no divided game FPS limiter |
| XeFG | XeLL and verified exact-build 4X unlock/pacing integrated; SDR/PQ resize/recovery passed | Gameplay pacing and GPU compatibility; 5X/6X remain deferred |
| FSR FG | 2X SDK presentation with SDR, PQ and scRGB; resize/recovery passed | Gameplay/NR combinations and Radeon Anti-Lag 2 hardware testing |
| NR precision/runtime | Post-SR SDR pipeline integrated; 0.2.3 confirmed in-game, both patched versions pass live GPU switching tests | Game image/performance and other GPU generations; kernels remain runtime-defined, no claimed FP8 override |
| NR multipass | 1–8 independent native features, per-pass profiles/preset hints/tuning, feedback and GPU retirement implemented; two/three-pass hardware tests passed | One history per pass, creation submission completion, safe retired models and descriptor lifetimes |
| NR model scale | 25–200% live requests, recreation debounce, residual composition and budget admission implemented; 50% GPU probe passed | Game quality/VRAM checks at realistic resolutions; estimates are not hard allocation guarantees |
| Pause NR on focus loss | Integrated and full-pipeline GPU pause/resume test passed | Confirm OS focus transitions and background power behavior in-game |
| Foveated NR | Symmetric peripheral compression, guide/endpoint-motion transforms and matched residual implemented; WARP reference tests and both model GPU tests passed | Matched inverse mapping, endpoint-transformed motion, preserved center, HDR/PreSR bypass rules |
| Bottleneck reuse | Interval 1–4 and first-pass policy configuration implemented | 1 means full evaluation; every output frame still evaluates NR; verify caches, kernels, pass order and resets independently for intervals 2/3/4 |
| Unverified kernels | Explicit opt-in setting implemented; launch checks pending | No guessed buffer slots, no stale cache after feature/runtime changes, bounded fallback when launch ordering differs |
| Generate before SR/apply after | Configuration implemented; residual backend pending | Matched clean/edit grids, actual raster jitter, trailing/disocclusion rejection, no global state contaminating ordinary DLSS |
| Scene HDR | Parallel RenoDX scene capture, native linear-HDR DLSS, HDR-aware NR and UI transmittance implemented; controlled GPU tests passed, game test pending | SDR and HDR input domains explicit; correct UI alpha, opaque previews, transparent HUD and complete native AA |
| HDR output | 10-bit PQ and 16-bit scRGB actual swapchains, native SDR carrier and ReShade/REST hardware tests passed | Format/color-space tests per FG provider and display; no false 12-bit DXGI format |
| HDR controls | Validated persistent settings, live controls/resets and highlight preservation implemented; game test pending | Stable brightness on HUD transitions, effects-compatible SDR bridge and highlight preservation |
| HDR screenshots | ICC profile/hotkey/game screenshot request types implemented; capture pending | Correct actual presented planes/UI, PNG profile/chunk validation, independent standards/ICC-only options |
| REST HUD isolation | Compatibility contract pending | Native capture timing, current-frame identity, format conversion accounting, no whole-screen exposure pumping |
| Previews/portraits | Instance limits 1–4 implemented in settings; renderer pending | Complete material passes before capture, independent views/guides, 2X output relative to actual native extent, GPU-safe cap/retirement |

## Removed

No alternating NR-evaluation mode is defined. Bottleneck intervals refer to cached bottleneck computation, not skipping NR frames. No approximate/synthetic FG guides are defined. Preview FG is not in the requested retained feature list and will not be ported speculatively before preview DLSS/NR and main presentation are stable.

## Order

1. Validate native DLSS quality/model/jitter, mip bias and depth in-game.
2. Bring up isolated single-pass NR using the explicit DX11/DX12 transport; validate actual game input contracts.
3. Build the presentation owner on that transport for Streamline/Reflex, DLSS-G and dynamic factors, then extend NR memory/reuse/PreSR/foveated controls.
4. Add scene HDR, effects compatibility, output negotiation and screenshots; validate all input domains together.
5. Add REST integration and complete preview/portrait processing under the shared budget.
6. Bring up XeFG and explicitly experimental multipliers with provider-specific latency/output restrictions.

A single successful build is not any of these gameplay acceptance results.
