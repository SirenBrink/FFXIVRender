# Scene HDR, 0.4.9

The FFXIV RenoDX pixel shaders are replayed from the game's original unclipped FP16 scene data into an independent FP16 branch. The original SDR rendering remains available for existing ReShade effects. Captures follow recognized tone mapping, post-tone mapping, LUT and gamma stages; only verified fullscreen passes and exact resource aliases are accepted. Native quality, raster jitter, depth and motion remain authoritative.

Native DLSS is created with IsHDR and evaluates linear HDR input once. A call-local parameter overlay changes only private color/output resources and their zero-based subrects. Each game-owned feature retains its output until successful native ReleaseFeature. NR edits a separate working copy; neither NR nor SDR encoding can overwrite DLSS's HDR history. As of 0.4.9, HDR is live: a successful output transition commits the scene domain, and the native render thread requests DLSS recreation when that domain changes. Gameplay validation of transitions during scene loading remains outstanding.

NR receives either a bounded reversible scene-HDR proxy or the SDR model-input option. Relative edits are mapped onto the untouched linear HDR source; identity preserves highlights, negative reconstruction overshoot and original alpha. Multipass, model resolution, peripheral compression and live runtime changes use this same path. This is not a claim that the NR network itself evaluates unlimited linear luminance.

For presentation, a genuine native SDR swapchain on a separate private hidden window retains the exact game/ReShade UNORM and sRGB view rules. It is never presented. The single actual Present goes to a separate flip HDR swapchain, after ReShade effects and overlay. There is no extra DLSS evaluation or extra presented game frame. The selected output is either R10G10B10A2_UNORM with BT.2020 PQ, or R16G16B16A16_FLOAT with linear BT.709/scRGB (1.0 = 80 nits). There is no distinct 12-bit DXGI format.

Opaque and alpha-blended UI draws accumulate actual native transmittance in an R8 mask. Scene highlights are peak-mapped before UI transmittance is applied. Optional highlight preservation tracks ReShade's pre/post effect colour change in the SDR image while retaining the independent scene lighting. Arbitrary nonlinear/spatial ReShade effects still need visual testing; SDR compatibility is not mathematical equivalence to running every effect natively in HDR.

If no recognized scene matches the DLSS input, status explicitly says it is waiting. The fallback only decodes SDR for display; it does not invent highlights or count that as recovered scene HDR. The overlay and periodic log include observed stage IDs, capture count, matched DLSS inputs and missing matches. These counters are important for validating game builds and different configurations.

## Controls and first test

Enable Windows HDR. In XIVRender, enable Scene HDR and select 16-bit scRGB or 10-bit PQ; these changes apply live. Do not load the official RenoDX HDR addon simultaneously. Begin with NR disabled; check world lighting, title/selection, opaque previews and transparent HUD. Test Game/native DRS, DLSS Quality/Balanced/Performance and model changes, then NR with each runtime, multipass, and the NR scene-HDR input checkbox. Check ordinary ReShade effects, opening/closing menus, resize and alt-tab. Save settings and exit normally; XIVRender.log records HDR match statistics.

The HDR master/output and peak/paper white/black/contrast/saturation/vibrance controls apply live. NR scene-HDR input and ReShade highlight preservation are live. DLSS-G/XeFG accept PQ10; FSR FG also accepts scRGB16. Screenshot capture and preview enhancement remain incomplete.

## Validation

CTest covers distinct HDR lighting values behind identical clipped SDR references, private native DLSS history, separate simultaneously live feature outputs, opaque-UI occlusion, identity ReShade effects and exact NR HDR identity. The native interception fixture verifies IsHDR at creation, restored game parameters, original raster jitter and exactly one native evaluation.

Hardware probes exercise both genuine HDR output formats/color spaces, the native SDR backbuffer contract, Present/resize/teardown, and the installed ReShade/REST copies. Acceptance requires successful ReShade effect-runtime initialization and no ReShade errors. A real ReShade view-creation failure in the initial plain-texture proxy was found and corrected by retaining a genuine native SDR backbuffer.

The production NR hardware probe covers both compatibility runtimes, two passes and peripheral compression with both NR input policies on bright linear HDR, including finite RGB and exact alpha checks. These controlled tests do not establish in-game scene recognition, image quality, frame pacing or other GPU generations. First game confirmation is still required.

Shader source/provenance: https://github.com/clshortfuse/renodx/tree/main/src/games/ffxiv
Design reference: https://github.com/clshortfuse/renodx/wiki/Guide:-Using-RenoDX-to-Upgrade-HDR-in-Games

## Frame-generation format boundary

The current NVIDIA DLSS-G and Intel XeSS-FG programming guides explicitly exclude FP16/scRGB HDR. Both use R10G10B10A2_UNORM with HDR10/BT.2100 PQ for HDR presentation. XeFG additionally requires the backbuffer, HUD-less color and UI-only image to share that format/color space; Intel recommends its backbuffer/HUDless/UI composition mode when two-bit alpha is insufficient. SR and NR can continue processing FP16 scene data before the final HDR10 boundary. A later FG integration must select a supported presentation path; 16-bit display output with either FG provider is not claimed or validated.

Sources: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md and https://github.com/intel/xess/blob/main/doc/xess_fg_developer_guide_english.md

## 0.3.1 native discard backbuffers

The 0.3.0 game log showed HDR setup failing before the first native DLSS feature, which correctly stayed SDR (flags 10, hdr=0). Reproducing the game's BufferCount=2, discard swap effect and ALLOW_MODE_SWITCH flag isolated DXGI_ERROR_INVALID_CALL at SDR backbuffer acquisition. A discard chain exposes only index 0 even when its descriptor counts two buffers. The proxy now acquires only that accessible buffer while preserving the native descriptor count. The hardware regression exercises both counts, both HDR formats, count-changing resize and exact native buffer-1 rejection. Setup failures now name the failed stage and HRESULT instead of suggesting Windows HDR was necessarily disabled.

## 0.3.2 presentation ownership

The next user log returned E_ACCESSDENIED (0x80070005) at native swapchain creation. The earlier compatibility and HDR chains shared the game HWND. The current transport puts its never-presented SDR chain on a separate hidden HWND, preserving real DXGI UNORM/sRGB view compatibility without competing for the display window. Both private SDR and actual HDR chains deliberately use flip presentation internally; the game/ReShade still see their original logical discard descriptor and accessible buffer 0. Only the HDR chain is presented. This avoids relying on a driver retaining the private SDR chain in blt mode.

The hardware probe now confirms distinct HWNDs, an invisible private transport, rejection of a competing flip chain on the occupied display HWND, 4K and zero-sized native creation requests, one/two logical buffers, explicit/automatic client resizing, and private-window destruction. An explicit collision reproduces the access-denied class of error; it does not establish which driver/overlay behavior led to the user's previous failure. Setup errors now distinguish SDR carrier creation from HDR display creation. First game confirmation remains required.

HDR output selection remains explicit: 16-bit scRGB is reserved for operation without FG, and 10-bit HDR10/PQ is the required choice for the planned DLSS-G/XeFG paths. FSR FG is deferred along with the other FG implementations until HDR works in-game.

## 0.3.3 ReShade/native identity at the DLSS boundary

The 0.3.2 log confirms HDR presentation and HDR feature creation but zero completed DLSS evaluations. Preparation returned before native Evaluate because swapchain warmup stored a native D3D11 device while the game supplied ReShade's wrapped context/device. The previous direct pointer comparison rejected the same underlying device. This was reproduced with installed ReShade/REST by preparing HDR through the wrapped context after native HDR swapchain setup.

HDR now resolves the documented explicit native-object interface for device identity and uses its actual immediate context consistently for context hooks and HDR commands. A genuine second device still fails, even on the same adapter. Preparation failures now increment the DLSS failure counter and identify their reason instead of silently returning before telemetry.

The expanded integration probe performs a synthetic tagged scene draw through ReShade, verifies native replay/capture, prepares the private HDR DLSS input, simulates an HDR DLSS output, isolates the NR working image, converts back to the native SDR-compatible output and reads all four full-resolution corners. It tests the HDR/SDR plumbing; the model output in this probe is simulated, not an actual NVIDIA SR evaluation. Actual in-game DLSS operation and recognition of FFXIV's real scene shaders require the next run. Shader/draw/match counters and rejection reasons are retained in periodic HDR status to make that distinction observable.

## 0.3.4 deferred scene and UI recording

The 0.3.3 game log confirms successful HDR presentation and native HDR DLSS evaluation, but recognized scene shaders never reached the immediate-only draw capture. Just 29 native draws were observed while thousands of SR frames completed. An isolated deferred-context draw reproduced the missing capture before this change.

Capture now attaches to native immediate and deferred contexts through ReShade's init_command_list callback. Native FinishCommandList retains recording metadata on the command list; ExecuteCommandList publishes aliases only after queuing the actual GPU work. Discarded/unexecuted lists never become valid scene inputs. Stable per-resource HDR/reference storage connects recording contexts; dependency validation follows execution order rather than CPU recording order. Copies propagate the HDR branch and native clears invalidate it. Command-list metadata pins storage for cached replay. The bounded scene cache releases idle, unpinned resources after resolution changes.

UI transmittance uses the same deferred recording route, clearing its mask once per executed frame rather than once per recording context. Separate translucent layers multiply; opaque UI blocks world highlights. The module stays loaded while private COM metadata can reference its code.

Hardware acceptance now requires actual ReShade deferred-context creation callbacks, execution order, GPU reference-copy readback, cached-list replay across frames, discarded-list exclusion, clear invalidation, translucent accumulation and opaque masking, with both HDR formats and native backbuffer counts. The probe uses a synthetic tagged scene shader and simulated DLSS output; it does not establish recognition of the current game's complete real shader chain. The next game log must show deferred draws, captures and DLSS matches before full-scene HDR is considered confirmed.
