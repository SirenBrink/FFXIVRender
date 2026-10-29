# Repair build 0.4.16 — FSR multi-frame generation

**Pre-release gameplay status: FSR-FG through DLSS Enabler is currently not working.** The controlled test results below are historical development evidence, not gameplay acceptance.

The supplied version.dll is a proxy build, not an official headless distribution. Its proxy callback installs process-wide behavior and forcibly terminates the process on detach. `tools/prepare-enabler-headless.py` verifies the exact original SHA256 and eight original instruction bytes, then derives a private copy that jumps to that callback's normal success epilogue after module-handle setup. PE/CRT initialization, constructors, TLS and explicit DLSSG APIs remain unchanged. The original supplied file is preserved. This adaptation restores normal ReShade/REST shutdown in the controlled comparison; a renamed unmodified copy is not accepted for deployment. The manifest records both hashes and the exact adaptation.

The user confirms that 0.4.15 XeFG now competes with DLSS-G. Its submission timing and presentation flags remain intact, as do the working DLSS-G and classic FidelityFX 2X paths. Companion stays disabled.

FSR's optional DLSS Enabler backend supports 2X/3X/4X with the user-supplied 4.9.0.15 headless runtime. The original game DLSS loader is untouched. A bounded three-group worker owns the real swapchain, preserving input/output lifetimes while presenting interpolated images in order. Independent latency controllers receive actual worker Present markers. Provider changes, resize and native-source retirement drain the presentation worker and both GPU fences before freeing images or SDK state. SDR and HDR10/PQ are supported; scRGB is explicitly rejected for this new backend, while classic FSR 2X retains scRGB support.

The initial integration's command ownership failure was reproduced with the D3D12 debug layer. Inspection of the supplied runtime established that `DLSSG.IsRecording=1` is required when the caller owns the recording/submission. The runtime otherwise resets and closes the command list. The corrected moving-bar GPU test checks actual readbacks: 3X has two distinct intermediate positions, and 4X has three equally spaced positions before the current real image. Present counts alone are not treated as proof of interpolation.

`tools/test-enabler.ps1` verifies final-binary SDR pixels, live PQ provider switching, and combined two-pass NR/PQ/FG. Installation requires its exact binary/runtime receipt as well as the existing ReShade NR/HDR/output checks. These controlled results do not certify FFXIV image quality or performance.

PreSR and the remaining backlog were audited in `research/pending-feature-audit-0416.json`. They are not enabled by this build. PreSR requires a separate pre-upscale NR/reconstruction history and matched HDR residual composition before FG capture. REST requires a proven pre-HUD marker and current-frame identity, including rejection of its late Present fallback; merely accepting a marker texture would risk interpolating HUD or discarding HDR highlights. HDR screenshots, preview/portrait processing and kernel-observed bottleneck reuse likewise remain separate pending implementations.

# Repair build 0.4.15 — XeFG private-device timing

The user confirmed that disabling FFXIV OptiScaler Companion removed DLSS-G and FSR-FG stuttering. XeFG flicker is also resolved after 0.4.14, but its pacing remains poor. The Companion stays disabled; no working DLSS-G or FSR presentation policy is changed.

The new gameplay recording shows XeLL waits averaging 33.51ms (99th percentile 74.44ms) in native 2X mode without the unlock. FFXIV presents the preceding simulation's image. Previously, XeLL's render-submit interval began during the native main update and ended at the following tick's private DX12 Present, spanning the following simulation's sleep. XeFG now starts that interval when recording/tagging the actual private DX12 submission, using the image's frame ID. Standalone XeLL retains caller-provided markers. Windowed, unsynchronized XeFG Presents also receive ALLOW_TEARING, matching the private swapchain's creation flags; VSync and other providers retain their existing behavior.

These are targeted candidate repairs, not gameplay acceptance. The hidden test now supports a one-simulation-delayed image pipeline to exercise the production ordering. Research into DLSS Enabler's real FSR-based 3X/4X/6X implementation is recorded in `research/xefg-fsr-mfg-0415.json`; it requires a different NGX-based integration and presentation path and is not enabled by this build.

# Repair build 0.4.14 — XeFG descriptors and historical periodic-stall investigation

The user's 0.4.13 run still has periodic DLSS-G/FSR stalls and XeFG camera-motion flicker, despite improved XeFG behavior. The recording confirms approximately two-second intervals between 60–90ms frame gaps. Sampled provider Present calls at those gaps are mostly under 1ms. The previous trace leaves main-thread game work and the renderer-update prelude indistinguishable. Evidence is archived under `diagnostics/fg-0413-gameplay/periodic-stall-analysis.json`.

XeFG now uses SDK-owned descriptor storage. The previous single external descriptor region was reused across frames without retirement of the preceding frame's interpolation before the next descriptor binding. Intel's developer guide requires descriptor storage to remain valid until its GPU use ends. Removing the external-heap flag and binding delegates that lifetime to XeFG. This addresses a concrete lifetime risk; camera-motion image quality still requires gameplay validation.

New sparse timing identifies slow latency markers, frontend policy, settings requests and effect restoration; a renderer-prelude scope separates those calls from native Framework time. Sparse timing reports only calls of at least 2ms and is labelled separately from complete distributions. The two-second stall is **not claimed fixed**. Existing settings and XeFG pacing corrections are retained.

# Repair build 0.4.13 — OptiScaler audit

The local OptiScaler integration already contained device-specific NGX shutdown, XeFG context recreation after native upscaler replacement, time-based dynamic multiplier hysteresis and fence validation after waking. These are now implemented in the addon. The detailed audit, including existing coverage and intentionally excluded changes, is in `research/optiscaler-addon-audit-0413.json`.

The shutdown guard routes legacy/null shutdown through the recorded game DX11 device, preserving independent DX12 NGX users. Failed calls remain retryable; completed game shutdown is idempotent. XeFG recreates its context at the next safe presentation handoff after native source retirement. Dynamic fallback changes require sustained headroom and cooldowns. Fence waits reject device-removal sentinels even after their event wakes.

Validation: all 21 C++ suites pass. The real NVIDIA driver probe verifies repeated native DX11 shutdown followed by successful use and close of the separate DX12 Streamline/Reflex instance. The 200-frame hidden provider test verifies source retirement, XeFG context recreation, resizes, provider changes and return from Off. Native DLSS hook/quality contracts and ReShade NR/HDR integration also pass. Evidence is under `build/optiscaler-audit-*`.

Gameplay pacing and normal FFXIV exit remain unconfirmed. Hidden DLSS-G tests validate lifecycle, not foreground interpolation. The sections below retain earlier build evidence and limitations; they are not acceptance results for 0.4.13.

# Repair build 0.4.10 — historical scope and evidence

This candidate is built in `build/XIVRender.addon64`. Deployment history and backups are under `deployments/`; installation does not constitute gameplay acceptance. The 0.4.9 source files changed for this repair were preserved under `diagnostics/fg-049-gameplay-20261028-2048/source-before-0410`.

## Actual 0.4.9 gameplay result

The user reported persistent DLSS-G stutter, poor XeFG behavior, periodic FSR FG stutter and a slow exit that required force-closing FFXIV. The stutter continued with the settings overlay closed. XeFG did not leave FPS permanently reduced after switching away in this run. Preserve that improvement without treating it as a pacing or shutdown pass.

The source recording and logs are archived at `diagnostics/fg-049-gameplay-20261028-2048`. Many slow frame intervals include time outside the recorded vendor Present and latency-sleep calls. The old recording cannot attribute that time to a specific game/ReShade stage. FSR's approximately 548 ms outlier coincided with NR initialization; that does not explain its repeated shorter stalls. A later XeFG multiplier change coincided with a 3.74-second Present. The old recording ends without an explicit shutdown marker, so its zero unfinished Present calls never certified orderly exit.

Two NVIDIA Streamline crash dumps are preserved in that archive's `streamline-crashes` directory. The first is an execute access violation from a cached NGX model into an address absent from the mapped-module list. Its offset and logging-call arguments are consistent with a callback into an unloaded `sl.common` image. The second contains an exception during the subsequent Streamline retry. These support a callback lifetime defect; they do not establish the cause of every periodic stall or the forced exit.

## Changes in 0.4.10

- During Streamline device binding only, replace its NGX logging callback with a host-owned callback. Match the binding thread and exact Streamline project ID; leave foreign initialization unchanged. Disable the init detour immediately after binding. Keep the callback's code, driver-core image and disabled trampoline resident for process lifetime so cached callbacks and in-flight foreign forwards remain valid. This retains no FG device, swapchain, model or worker context.
- Supply XeFG's documented `frameRenderTime=0` automatic timing value. The former estimate excluded scheduler sleep and was not the complete frame interval. The CPU estimate remains diagnostic data, not the SDK timing input.
- Record native Framework/renderer work, ReShade effects/callbacks, wrapped output Present, native DLSS release and NR/output/presenter shutdown. A terminal marker distinguishes completed recording shutdown from an abruptly ended trace. The analyzer separately reports inter-Present intervals, phase timings and incomplete scopes.

The callback interception has regression coverage for cached callbacks across four retirements, exact restoration of init bytes, immutable caller input and foreign initialization. The analyzer verifies that cheap Present calls can coexist with a long frame interval and that an incomplete shutdown remains visible.

## 0.4.10 validation

The final build passed all 20 automated suites (`build/repair-0410-final-build.log`) and all three Python analyzer tests. Hardware checks with the installed ReShade and REST passed both NR runtimes/live controls (`repair-0410-nr-reshade.log`), HDR composition/resize/teardown (`repair-0410-hdr-reshade.log`) and twelve output/provider phases (`repair-0410-output-reshade.log`). The output recording contains four successful callback replacements, four detour-disable completions and a clean terminal marker, with no record loss, parse errors or unfinished calls (`repair-0410-output-analysis.json`).

The 650-frame settings/provider stress test and the combined test with 630 steady two-pass NR frames also passed (`repair-0410-fg-stress.log`, `repair-0410-nr-handoff.log`). Both recordings ended cleanly, with no record loss, parse errors or unfinished calls. NR reported no new omissions, nonfinite RGB or alpha errors. XeFG and FSR reported generated frames; the hidden window did not require DLSS-G interpolation. DLSS-G device binding still took approximately eight seconds, and first activation had approximately 1.2-second Present outliers. These transition costs remain.

NGX feature-cache lookup/shared-memory-open failures, Streamline plugin-discovery/duplicate warnings and the GetState synchronization reminder remain preserved in the recording. Successful tested initialization does not make these messages disappear. The NR/HDR probes still print NVIDIA's `[CDataFile::Save] No filename has been set` cleanup message. Hidden hardware tests cannot establish focused DLSS-G generation, FFXIV frame pacing, or a normal FFXIV exit.

The regular gameplay stalls and forced exit remain unresolved until a new FFXIV run verifies behavior and the expanded recording identifies any remaining delays. Existing REST, HUD isolation, presets and settings must be preserved during deployment.

## Prior 0.4.9 implementation and synthetic evidence

The remainder records the preceding build's changes and tests. Its predictions were superseded by the actual gameplay result above. The original source snapshot is `repair-backup-20261029`.

## Findings from actual gameplay logs

The local `diagnostics/fg-046-20261028`, `fg-047-20261028` and `fg-048-20261028` snapshots contain repeated Streamline frame-timer resets after frames exceeding 100 ms, often roughly two seconds apart. In the 0.4.7 session inside `fg-048`, ordinary FG-off presentation remained around 23 FPS with effectively zero latency-sleep time and roughly 24–42 ms average Present time. This locates the persistent delay in presentation; it does not establish XeLL sleep as the cause. The most recent installed session had no accepted native captures and cannot validate interpolation.

The old SDK callback overwrote a status string, so periodic summaries could report zero failures while Streamline emitted warnings. Those messages now persist independently in [always-on recordings](FG_DIAGNOSTICS.md).

## Implemented repairs

- Temporary DLSS-G input gaps retain internal resources and the last valid dimensions. Previously Off released resources, causing reallocation when the next valid frame resumed. NVIDIA documents that transition as a stutter source. Multiplier/dynamic/target edits stay on the active SDK.
- Provider handoffs now destroy the SDK session/context and private presentation device. The previous design retained Streamline common/Reflex state and pinned XeFG patches. XeFG hook/capability bytes now restore after context destruction and refuse to overwrite foreign edits.
- Native DLSS/DRS is an explicit default, independent of model controls; automatic mip bias is opt-in. Native guide subrects and jitter remain available for depth and FG. Existing saved quality IDs are preserved.
- SDR/PQ10/scRGB16 outputs can change while retaining ReShade's original game buffers. Scene HDR commits after successful output initialization; the native DLSS feature rebuilds when the committed input domain changes. Failed output requests retain the previous transfer function.
- The supplied pinned SM75/SM86 bootstrap is staged and selected by the rendering adapter's LUID/CUDA architecture. It is initialized before Streamline with host-owned controls. Its exact DLL hash is verified.

These are demonstrated code/lifecycle defects and repairs. The periodic foreground DLSS-G stutter and permanent in-game slowdown are **not yet confirmed resolved** by gameplay evidence.

## Validation and its limits

The build passes 19 automated suites, including persistent concurrent diagnostic capture, UTF-8/escaping, visible writer failures and recovery. Two Python analyzer tests verify warnings survive normal status/rotation and that malformed/lost/incomplete evidence is not considered clean. GPU tests additionally exercise actual SDKs, native DRS subrect changes, output resizing, provider/latency changes, real two-pass NR continuity and ReShade/REST integration.

The final combined NR handoff run (`repair-final-nr-fg-handoff.log`) completed 630 steady two-pass NR frames with no new NR omissions/failures, finite RGB and preserved alpha. XeFG and FSR produced generated-frame observations. Streamline initialization during handoff took several seconds; this remains a one-time transition cost. The recording had zero dropped records, parse errors or unfinished Present/sleep/handoff calls.

The six-cycle `xefg_lifecycle_probe` compares every executable-section byte against its pristine snapshot after restoration, checks module unloading, and verifies refusal to overwrite a simulated foreign hook. This supplements actual SDK context-destruction tests; it does not simulate Intel worker concurrency.

| Validation | Evidence under `build/` | Result |
| --- | --- | --- |
| Final C++ build and 19 suites | `repair-final-validation-build.log` | Pass |
| Six XeFG exact-byte restoration/unload cycles | `repair-final-xefg.log` | Pass |
| 650-frame SDR stress through production Output | `repair-final-fg-sdr-stress.log`, `sdr-stress-analysis.json` | Pass |
| 650-frame PQ stress through production Output | `repair-fg-settings-stress-2.log`, `pq-stress-analysis.json` | Pass; before final teardown-result logging additions |
| 630 steady two-pass NR frames across provider handoffs | `repair-final-nr-fg-handoff.log`, `final-nr-handoff-analysis.json` | Pass |
| Both NR runtimes and live host controls with ReShade | `repair-final-nr-reshade.log`, `nr-reshade-passed.json` | Pass |
| HDR composition, PQ/scRGB outputs and resize with ReShade/REST | `repair-final-hdr-reshade.log`, `hdr-reshade-passed.json` | Pass |
| Twelve output/provider phases with ReShade/REST | `repair-final-output-reshade.log`, `output-switch-reshade-passed.json` | Pass |

The first stress run is retained as `repair-fg-settings-stress.log`: its deliberately missing capture failed the camera-cohort assertion. The corrected test supplies an invalid guide while still consuming that evaluation's camera, matching the production failure path; it does not relax the assertion.

The corrected PQ recording contains two slow initial DLSS-G activations (1.17/1.39 seconds). During setting edits and gap recovery, subsequent valid-frame Present calls stayed below 0.68 ms in this hidden workload. Following direct DLSS-G → XeFG → FSR → Off transitions, mean Present time was about 1.17/0.26/0.24 ms respectively. SDR also completed the sequence without a persistent 24 FPS-style Present delay. No recurring 100 ms frame-timer-reset warning, API failure, record loss or unfinished call appeared in these recordings. Vendor plugin discovery/duplicate warnings and the GetState synchronization reminder remain recorded; GetState calls are serialized with Present. The ReShade NR/HDR probes still print NVIDIA's `[CDataFile::Save] No filename has been set` cleanup message. None of these results certifies foreground interpolation or gameplay frame pacing.

Hidden probes cannot establish focused DLSS-G generation or FFXIV smoothness. The available NVIDIA system reports SM120; RTX 3080/SM86, SM75, Ada and Radeon acceptance remain outstanding. Anti-Lag availability is reported accurately on unsupported hardware.

## Outstanding requirements

SM75/SM86's external compatibility module has no safe unload contract and remains resident; complete removal of its opaque hooks is not implemented. The Ada MFG unlock, FG model-preset overrides, NR pre-SR generation and ViT bottleneck reuse have not been ported. FSR currently provides 2X, not 3X+. Those saved/visible requests must not be mistaken for implemented behavior. SDK and OS/hardware capabilities still constrain available combinations.

Final acceptance requires the exact staged build in FFXIV with foreground DLSS-G setting edits, DLSS-G → XeFG → FSR → Off → DLSS-G, native DRS and fixed DLSS presets, SDR/PQ/scRGB where supported, NR off/on/multipass, and inspection of all recorded warnings and timing windows. Current synthetic passes do not replace that acceptance.
