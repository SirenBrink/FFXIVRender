# First game run: 2026-10-24

The initial 0.1.0 addon registered successfully in ReShade 6.8, resolved the current FFXIV signatures and intercepted native DLSS feature creation. The user's saved selection ended at Balanced with model M. The user could not confidently verify the visible changes, so this is startup/interception evidence rather than confirmation of correct live switching.

The log exposed three gaps:

- Creation logs could reuse the previous evaluation's subrect keys from a reused NGX parameter block. Feature-creation contracts and evaluated active subrects now have separate sources. The UI prominently displays Input and Output from the last successful world evaluation. A regression fixture covers stale creation subrect metadata and an active-size change without creating another model.
- An NGX-native/proxy context view tried to attach automatic mip handling again, producing an already-attached warning. Main swapchain setup now owns initial attachment; evaluation only falls back if no attachment exists. Final main swapchain destruction releases retained mip/depth resources before ReShade device cleanup. This lifecycle behavior still needs a game test.
- There was no final addon summary. A summary is now recorded periodically and at main swapchain destruction, as well as AddonUninit. Concurrent native evaluation/present status counters are modified under one transaction rather than overwriting stale status snapshots.

Diagnostic build 0.1.1 logs request names/IDs, native size-query answers, native resize dispatches, feature contracts and actual evaluation dimensions/allocations. Bounded 48-frame windows record raster phase/frame/strength, queued jitter values and forwarded NGX jitter, plus motion scale, reset, exposure, timing and sampler-variant counts. Logs use a persistent stream with timestamps/thread IDs and periodic summaries; steady-state frame-by-frame tracing stops after the window.

All nine focused suites pass. Real image quality, live model selection, observed native jitter alignment and final teardown behavior remain pending the next in-game run. NR/FG/HDR are still inactive in this milestone.

## Second run and 0.1.2 correction

The user confirmed changing only addon controls. DLAA successfully created and evaluated at 3840x2160. The subsequent Ultra Quality request was followed by FFXIV leaving native DLSS mode (2 -> 1), with no further DLSS evaluations. The runtime-check button performs a file scan and has no DLL reload or graphics-option mutation; its status update is now limited to the runtime field so it cannot overwrite concurrent render status.

A separate D3D11 hardware probe on the RTX 5070 Ti reproduced the key trigger with the game's actual NVIDIA SR 310.9.16.0 DLL: raw NGX Ultra Quality (4) reports success but returns an optimal extent of 0x0. This invalid answer is consistent with the game's fallback; the in-game causal fix still needs confirmation.

The addon now intercepts the initialized SR runtime's real optimal-settings callback, using its own parameter map without reinitializing or shutting down the game's runtime. Main-world sizing queries receive the same contract as the game-side query. Ultra Quality temporarily queries/creates supported Quality (2), using a custom 77% input extent, then restores caller-owned quality and model-hint parameters. Preview/foreign extents and disabled controls retain their native behavior. Native quality requests wait until all SR callbacks are safely intercepted.

The corrected hardware callback probe returns nonzero 4K-input choices for all six modes: Performance 1920x1080, Balanced 2258x1270, Quality 2560x1440, Ultra Performance 1280x720, Ultra Quality 2956x1662, DLAA 3840x2160. Regression fixtures cover DLAA -> Ultra Quality transitions, temporary parameter restoration, custom feature creation and cached/renamed runtime discovery. All nine focused suites pass. These checks validate callback behavior, not the final in-game image.

Sampler variants are retained across temporary ReShade effect pauses; a WARP regression verifies twenty pause/resume cycles create no additional variants. The runtime-check tooltip states that it is read-only, and DLSS frame reception no longer remains active indefinitely after evaluations stop. The last successful resolution stays visible and labeled accordingly.

The native game's scale gate was inactive early in this run, delaying the initial saved reduced-resolution selection until it became active. No writes to that unverified gate or to the user's native graphics configuration were added.

## Third run and 0.1.3

The user confirmed the quality fix works. Log requests 11 (Balanced M -> L) and 13 (Quality L -> K) dispatched successfully but did not create a new native feature. The following quality changes, requests 12 and 14, created the requested models. FFXIV's optimal sizing cache at context+0x150 is separate from its applied 44-byte feature descriptor at +0xCC; invalidating only the first cannot recreate a same-size model. The current executable's descriptor comparison and native release routine were decompiled and structurally signature-checked. Model changes, activation refresh and disabling controls now invalidate only the cached numeric display extent using FFXIV's own uninitialized sentinel. FFXIV retains handle ownership and runs its normal release/create path. An isolated production-code fixture verifies recreation at unchanged dimensions, coalescing, deferral, restoration and no extra recreation for mip-only changes.

The run also contains one recovered NGX evaluation error during a quality transition (request 5, 0xBAD00005). This is not counted as a fully error-free session. Subsequent evaluations and the remaining mode switches succeeded; transition telemetry remains enabled for the next test.

The native GraphicsConfig+0x45 gate also disables scaling before login. A query-only override would be inconsistent: renderer sizing, native quality selection and postprocessing all read it. Native writers also change it for cinematic/event policy, so a permanent override would be too broad. The current executable was checked against FFXIVClientStructs GameMain: the direct singleton signature and a separate load-state reference agree, and native initialization/territory routines confirm the connected, 32-bit load-state, next-territory and current-territory offsets. Only a disconnected state with all three state/territory values zero qualifies for frontend scaling. A short lease spans game rendering until the main Present; it restores the original gate before the next frame's native event policy, on settings changes, teardown and frontend exit. Loading and connected-world states fail the frontend predicate; gameplay cinematic policy is untouched. Missing optional frontend signatures retain the native menu behavior and world DLSS controls.

This adds frontend scaling as a test feature; login, selection, login/logout transitions and live model-only changes still require user validation in the game.

## Fourth run and 0.1.4: ready lobby

The user confirmed live model changes work, but title/login and character selection still looked native. The 0.1.3 log supplies the exact cause: frontend state 0 initially ran Balanced at 2257x1270. At tick 2268109 the adapter logged leaving frontend scaling with connected=0, load state=4, next territory=0 and current territory=0. The subsequent native feature was DLAA at 3840x2160. The predicate rejected the ready lobby even though it still had no game zone.

This state is verified independently in the installed executable. AgentLobby's update function at current RVA 0x4CDB90 handles its UI-ready transition and writes GameMain load state 4 at 0x4CE0FC. FFXIVClientStructs documents zone states 1/2/3 but omits this lobby state. The audit is diagnostics/menu-load-state-20261024.txt, with the same executable MD5 as the other current analyses. These RVAs are research evidence, not new hardcoded runtime hooks.

0.1.4 accepts states 0 and 4 only while disconnected and both territory IDs remain zero. It refreshes the owned quality once at the 0 -> 4 boundary in case native initialization resets the selector. Stable lobby frames retain the current model. Regression coverage replays the observed transition, simulates a selector reset, verifies live quality/model changes in state 4 and checks restoration at Present. States 1/2/3/5 and unknown 32-bit values 0x100/0x104 remain excluded, as does state 4 with any connected/current/next zone field set. Native handle ownership and the working live model path are retained.
