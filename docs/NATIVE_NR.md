# Native NR compatibility boundary

The requested production selections are both user-supplied compatibility models:

| Runtime ID | Role | File version | SHA256 |
|---|---|---|---|
| `310.9.16-compat` | Default | 310.9.16.0 | `CDFA6C1EC784867182164C7836F324F087B5C9F374D0C9FE46F54999FEABF3ED` |
| `310.8-compat` | Alternative | 310.8.0.0 | `E67DEE209320CDAFE0E93E45675D7AA34323A53ACC57A72B2E40A181581C989A` |

Models live in `runtime/nr/<runtime ID>/nvngx_dlssnr.dll`. The unmodified 310.8 model is kept only as a diagnostic reference under `.cache/reference-nr/310.8-stock`, outside the production runtime. Its temporary 2.13 Streamline NR plugin is retained separately for reference and optional compatibility research. Neither is a hidden fallback from the two selected patched versions.

## Loader and ownership

`XIVRender_nvngx.dll_nr.dll` is an addon-private caller shim with distinct `XIVNR_*` exports. It loads the selected model by absolute path and invokes the model's D3D12 entry points directly. This avoids Streamline's model-loading route entirely. It does not patch Streamline, the NGX driver core, Windows signature policy or the model. The snippet checks its caller's module name; the shim's explicit name and real return frames satisfy that existing native calling convention.

Each loaded model generation has an explicit session, device and owned parameter block. The parameter implementation uses the SDK's typed virtual methods. It does not guess raw vtable slots or borrow FFXIV's mutable capability block. Populated callbacks belong to that exact model and must be discarded when the generation closes.

The shim validates PE64 input, required exports, exclusive model ownership, command-list device identity and feature ownership. Duplicate path spellings/file aliases cannot acquire separate owners for the same module. Close refuses live handles. Failed releases and partial creation handles remain owned for cleanup. The caller must submit/fence creation and evaluation work before releasing features or changing generations. The shim does not invent a GPU completion guarantee.

Streamline 2.14.16 can independently own FG/Reflex when their presentation backend is implemented. Native NR does not depend on a matching `sl.dlss_nr.dll` and cannot silently disable those other features when NR is unsupported.

## Evidence and limits

The isolated `native_nr_probe` loads each patched version on the local RTX 5070 Ti, creates a 512x512 feature 18, submits/fences initialization, evaluates a reset frame, submits/fences output readback, releases the handle and shuts down the model. Output starts with FP16 NaN sentinels so a no-op or partial RGB write cannot pass. Both return success for every native operation; each produces 786432 nonzero RGB samples with no NaN/Inf samples.

The same process and device also successfully tested **310.9.16 -> 310.8 -> 310.9.16**, with a fresh owned parameter block and fully retired work for each generation. Evidence is in `build/native-nr-patched-switch-evaluate.txt`.

This verifies direct loading, GPU execution and quiescent switching on this RTX 50 card. It is not an in-game NR image-quality test, a performance comparison, a switch during active gameplay, or verification on RTX 20/30/40. Those generations remain intended compatibility targets and require hardware tests. Precision and kernel support must come from the selected model's actual capabilities; the FP16 compatibility runtime must not be advertised as native FP8 hardware execution on older cards.

Native DLSS-G hardware support also differs by generation. Older GPUs need the planned alternate FG provider where NVIDIA's runtime declines support. Removing this NR loader obstacle does not establish identical FG capabilities across all four GPU generations.

Run the probe manually after building; it is deliberately not part of CTest or game startup:

```text
native_nr_probe.exe <absolute bridge DLL> <absolute 310.9.16 model DLL> --evaluate <absolute 310.8 model DLL>
```

The isolated probes do not change game files, game settings, global DLL search paths or NVIDIA application profiles.

## 0.2.0 game integration

Single-pass NR now runs after a successful native world-SR evaluation. The original game NGX parameter block and raster/NGX jitter remain read-only. Post-SR color is paired with active native depth/motion regions; guides are unjittered and motion retains the native screen-space units through conversion to the model grid. Native HDR or jittered-motion contracts outside this first path retain the clean SR output.

An authoritative native D3D11 device comes from ReShade's supported device API, not a resource GetDevice hook. One shared FP16 color input/output, R32 depth and RG16 motion bundle is GPU-ordered across three command slots. Separate allocation slots are available for later independent FG packets. CPU writes/maps of aliased shared images are forbidden: a hardware/software regression demonstrated that CPU fast paths bypass cross-API GPU fence ordering. Current producers are compute dispatches and GPU clears/copies.

Runtime/resolution changes debounce, stop new NR submissions and poll old GPU work before releasing/closing the generation. Ordinary frames use GPU ordering without CPU completion waits. From 0.4.3, exhausted command slots apply at most 250 ms of CPU backpressure for the oldest already-ended D3D11 consumer, then recheck both model completion and consumer retirement before allocator reuse. This wait has no dependency on future Present or new frame submission: EndFrame already queued and flushed the completion wait, composition and retirement signal. The previous per-frame clean fallback caused NR to blink and repeatedly reset history under sustained queue pressure. A recovered slot now processes the current frame without resetting history or adding textures, slots or queued latency. Timeout latches a failure and keeps stable clean presentation until disable/retry/configuration change. It never reuses stale NR output. Failed generations retire as soon as fences permit. Turning NR off and teardown also retire D3D11 consumers before releasing transport images. Unknown completion retains owners rather than freeing GPU work still in flight.

Original SR alpha is always copied unchanged. NR alpha never controls scene/material/UI coverage. RGB edits are measured against the exact model input and composed over the clean high-resolution source, so lower model resolution retains native SR detail. Unwritten model output starts with GPU NaN sentinels; partial/nonfinite pixels fall back to clean SR.

Model creation checks the process's current DXGI video-memory budget against transfer/composition storage and a conservative model allowance. While active, the pipeline checks available headroom once per second and safely retires NR if less than 256 MiB remains. The clean SR frame is retained, the saved NR preference is preserved, and retry is explicit. These checks reduce avoidable allocation pressure; they do not guarantee that a driver or model allocation cannot exhaust memory.

The full `nr_pipeline_probe` passes 310.9.16 -> 310.8 -> 310.9.16, 50% model resolution, pause/resume and disabled retirement on this RTX 5070 Ti at 512x512, 1920x1080 and 3840x2160. Every checked output contains changed RGB, zero NaN/Inf and zero alpha mismatches. Evidence is `build/nr-pipeline-hardware.txt`, `build/nr-pipeline-hardware-1080p.txt` and `build/nr-pipeline-hardware-4k-final.txt`. All 14 automated test suites also pass, including rejected-start recovery through the enable toggle when no GPU owners exist. This is synthetic hardware validation; actual FFXIV appearance, quality transitions, VRAM cost and other GPU generations remain to be tested.

## 0.2.1 ReShade ownership correction

The first 0.2.0 game run reached native feature creation, but rejected its GPU completion fence before the first evaluation. ReShade 6.8 returns a device proxy from command lists/resources while its fences retain the original device. Comparing their unnormalized IUnknown pointers rejected two interfaces for the same device. The unregistered model recording then prevented generation retirement and all subsequent runtime/resolution changes.

The identical error was reproduced outside FFXIV using the genuine installed ReShade dxgi.dll beside the production pipeline probe. Identity validation now resolves ReShade's explicit IID_UnwrappedObject extension on both device interfaces, checks ID3D12Device and compares the underlying canonical IUnknown identities. It does not accept adapter equality or replace rendering interfaces. Unrecognized, cyclic or invalid wrapper chains remain rejected.

The pipeline now validates the completion fence before recording model work, preserves the exact submitted GPU fence/token if registration needs retry, and explicitly retires/reopens on Retry. A failed recording is never simply cleared or falsely marked complete. The corrected pipeline passes both model versions, live switching, focus pause/resume, 50% resolution, retry while an existing generation is ready, exact alpha and shutdown with genuine ReShade at 512x512 and 3840x2160. Evidence: `build/nr-with-reshade-before-fix.txt`, `build/nr-with-reshade-after-fix.txt`, `build/nr-with-reshade-4k-after-fix.txt`. All 14 automated suites pass, including completion-fence preflight rejection before any native model recording. Actual FFXIV appearance still requires the next run.

## 0.2.2 UI/render-thread ownership

The 0.2.1 character-selection log records one initialized model, at least 674 composed NR frames and zero reported NR failures before the user's crash on disable. No current Windows crash record or stack was available. The UI settings handler synchronously called Pipeline::Service(false), which can release the model, flush the engine's D3D11 immediate context and destroy interop resources on the UI thread. Present also serviced that retirement on the UI thread, while native SR evaluations ran on a different render thread.

Disable and retry now queue intent only. Native SR's render-thread callback consumes those requests and performs model/context work. An off/on cycle remains sticky until consumed, forcing a fresh generation even when both UI actions arrive before the next render frame. Present performs no NR runtime/context operations. A live generation refuses processing on a different render thread; clean DLSS is retained. Controlled teardown remains a separate shutdown path after rendering has stopped.

The production-host probe uses a persistent render worker and a separate UI caller, with genuine ReShade. It verifies that UI/Present disable leaves ownership intact until the render worker retires it, followed by re-enable, rapid off/on, retry, both live model versions and render-thread shutdown. Every checked RGB output changes while alpha remains exact and finite. This host test is now mandatory in the local install acceptance record alongside the direct pipeline test. The simpler isolated cross-thread retirement test did not itself reproduce the game crash; actual character-selection behavior requires the next game run.

## 0.2.3 REST cross-device destruction

`crash-20261025023058.tspack` provides the concrete failing stack: Bridge::Impl::~Impl -> ReShade -> ReshadeEffectShaderToggler+0x3C9DA -> +0x556A5 -> Direct3D11 execute access violation. The bridge return address +0xAF1AA maps to Release of the private D3D12 device (Impl offset0x18), not an image or fence. REST's instructions at those offsets match its global group-buffer cleanup followed by `device->destroy_resource`.

Upstream REST's onDestroyDevice calls DisposeGroupBuffers with the destroyed device and a global group map. Group resources created for the FFXIV D3D11 runtime are therefore released via the unrelated D3D12 device API. ReShade's D3D12 destroy_resource first treats that handle as ID3D12Resource, matching the cross-API failure in the stack. Dalamud reported the exception; its ReShade handler does not appear in the failing stack. The public handler's registrations concern Present, overlay and swapchain lifetime, rather than this device-destruction callback. No Dalamud/REST configuration is changed.

NR now uses ID3D12DeviceFactory through the optional D3D12GetInterface export, with DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON. It neither applies global SDK state nor modifies another addon's callbacks. The private compute device is not a ReShade game device; all textures, queues, fences and model generations still retire normally. Unavailable factory/driver support leaves clean DLSS and an NR error, without falling back to the unsafe wrapped-device route. Resolving the optional export dynamically preserves base-addon loading on older systems.

The probe now uses the same authoritative native D3D11 device as the game adapter. A test-only observer with genuine ReShade and the installed REST DLL records the old path exposing/destroying six private D3D12 generations (`build/nr-rest-device-events-before-ReShade.log`). The independent-factory path exposes zero private D3D12 lifetime events, while every NR model/version/control test still passes (`build/nr-rest-private-device-after.txt` and `build/nr-rest-private-device-after-ReShade.log`). This verifies removal of the callback trigger; the full FFXIV fatal exception was not recreated in the headless probe. The local install gate now requires observer and optional installed REST/config hashes. Test helpers remain excluded from the production whitelist.

## References

- Prior verified native caller route: SirenBrink OptiScaler fork, `OptiScaler/dlssnr/forwarder/dlssnr_forwarder.cpp`.
- Independent direct-model/owned-parameter implementation: https://github.com/maximedgoulet/FoveatedDLSS5/blob/main/src/addons/dlss5/dlssnr.hpp
- NVIDIA's current feature/hardware matrix: https://www.nvidia.com/en-us/geforce/technologies/dlss/
- ReShade 6.8 explicit proxy identity and original-device fence creation: https://github.com/crosire/reshade/blob/v6.8.0/source/d3d12/d3d12_device.cpp and https://github.com/crosire/reshade/blob/v6.8.0/source/com_utils.hpp.
- REST global cleanup: https://github.com/4lex4nder/ReshadeEffectShaderToggler/blob/main/src/Main.cpp and https://github.com/4lex4nder/ReshadeEffectShaderToggler/blob/main/src/ToggleGroupResourceManager.cpp.
- Microsoft independent device API: https://microsoft.github.io/DirectX-Specs/d3d/IndependentDevices.html.
- Dalamud public ReShade handler: https://github.com/goatcorp/Dalamud/blob/master/Dalamud/Interface/Internal/ReShadeHandling/ReShadeAddonInterface.cs.

## 0.2.4 NR controls and multipass

The 0.2.3 game run confirmed NR working without the previous crash. NR controls now carry typed model tuning into independent feature/parameter blocks per pass, within one exclusive model session. The first block is populated by the runtime; subsequent blocks copy those owned callbacks/defaults before receiving their own tuning. No game parameter block is used. All feature creation is submitted and fenced before evaluation; failure/partial handles retire through the same exact-fence ownership path.

Later-pass feedback reads the previous output, blends it against the untouched model-grid reference, and writes one reusable FP16 input image. Every later pass owns its own temporal feature. A frame's descriptor heap is immutable until its completion fence retires. The scratch image and model features survive until all work retires. Creation settings are debounced; feedback changes reset all pass histories, while final composition controls leave model history intact. No CPU frame wait or extra world DLSS pass is introduced.

Peripheral compression uses a symmetric rational map with continuous first derivative at the central region. Color and depth sample the inverse-mapped source; native motion endpoints are mapped into the same grid, including out-of-image endpoints. Composition applies the inverse-mapped residual over original DLSS detail, preserving native alpha. WARP tests independently invert the forward map by bisection and check centre/periphery color, depth, motion, identity, zero effect strength, and source chroma preservation. Real model tests cover 310.9.16 and 310.8 with two/three passes, feedback changes and compression; this is data-path validation, not a claim of in-game image quality.

Eight NR-only profile slots are atomic files. Loads validate before applying and retain the current runtime, enabled state and focus policy. Main settings include all active pass/profile/strength/compression controls. Per-pass preset hints are explicitly marked unverified, and no native-FP8 guarantee is made for compatibility-patched model DLLs.

Remaining NR migration: kernel-observed bottleneck reuse and unverified-kernel safeguards, PreSR/trailing controls, and scene-HDR mapping/input. These depend on their own native processing contracts and are not exposed as functioning switches yet. Alternating whole-frame NR and approximate FG guides remain deliberately absent.
