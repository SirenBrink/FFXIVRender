# Architecture decisions

## Ownership

ReShade hosts the addon and owns effect dispatch. FFXIV owns its native renderer, cameras, texture lifetime and real-frame identity. Native NGX owns native DLSS handles/history. Each optional backend must have an explicit device, queue, descriptor arena and completion owner. No process-wide loader redirection, guessed frame IDs or implicit shared model state is required by the foundation.

## Current modules

- `settings`: validated persistent requests, independent of runtime capabilities.
- `ffxiv_signatures`: bounded PE scanner and instruction/target validation, no memory writes.
- `ffxiv_hooks`: guarded native render-size requests and pre-raster/same-packet jitter adjustment.
- `native_dlss`: narrow, serialized NGX snippet entry interception; no private duplicate SR instance.
- `mip_bias`: immutable bind-time variants with logical/native sampler identity preserved.
- `depth`: one active-region raw-depth capture, temporary ReShade binding and restoration.
- `runtime_bundle`: read-only coherent SDK/model manifest, independent of actual activation.
- `native_nr_bridge` and `ngx_parameters`: direct model caller boundary with typed owned parameters, used by the post-SR NR integration. Both patched models use this route without weakening Streamline's loader.
- `dx11_dx12_bridge`: same-adapter shared NT images and explicit producer/model/consumer fence timelines. NR reuses one image bundle across three allocator slots; producers are GPU commands, never CPU maps/updates of shared surfaces. Nonblocking TryBegin remains available. NR uses bounded Begin backpressure only when all slots are retiring, preventing intermittent clean-frame fallback without adding image buffers or unbounded queue depth.
- `nr_engine`: private model generation, fenced asynchronous creation/evaluation and partial-failure retirement. It has no queue or per-frame CPU waits and does not borrow the game's NGX parameters.
- `nr_images`, `nr_pipeline`, `native_nr`: native guide cropping/unjittering, normalized motion, post-SR SDR NR and residual composition with exact original alpha; live model/resolution changes retire the preceding generation before opening another.
- `addon` and `overlay`: lifecycle, requests and honest status; backend state is not inferred from a checkbox.

## Presentation and Reflex

ReShade's public present callback is void and cannot replace/suppress native Present. DLSS-G for a DX11 game therefore requires a narrow native presentation bridge: shared DX11 resources feed an owned DX12 queue and a private FG proxy swapchain. Streamline manual initialization must occur before that chain's intercepted presentation calls. Native interfaces and Streamline proxy interfaces are distinct and must never be interchanged casually.

One real engine frame produces one immutable packet of guides, raster jitter, camera state and resource ownership. Generated presents consume that packet without advancing the renderer's jitter. Frame multipliers are observed at presentation, not assumed from requested factor. Ordinary FPS limits remain in output-rate units. Memory-pressure policy first removes redundant/idle allocations and declines optional preview growth; it does not add a ten-second FG-off timer.

Reflex must track actual simulation, render-submit and presentation boundaries. Adding sleep/markers only in a ReShade present callback is not full integration. XeFG uses its own supported XeLL pacing path instead of running two latency controllers simultaneously. Official non-Intel XeFG multiplier and HDR limits must be separated from experimental unlock behavior.

## HDR contracts

Every image packet declares its domain: scene-linear HDR, an SDR proxy/reference, PQ display output, or scRGB output. ReShade effects that expect SDR consume the SDR-compatible layer. Scene highlights and UI transmittance are composed only once at the agreed boundary. NR residuals are measured against the exact model input, and HDR DLSS outputs are not fed to a proxy encoder a second time.

DXGI has ordinary 10-bit packed PQ and 16-bit float scRGB paths. A 12-bit display wire mode is a driver/display negotiation, not a conventional 12-bit RGB swapchain format. The UI must never label a 10/16-bit buffer as 12-bit output.

## Patch safety

Resolve unique masked patterns from the installed executable. Validate PE ranges, executable targets, writable global destinations and the relevant call graph/unwind data. Before detouring compare the live instructions to the same disk image, including RIP-relative operands. Missing or ambiguous signatures disable the affected module. Structure/layout changes can invalidate an otherwise unique pattern, so 100% future-patch uptime cannot be guaranteed.

FFXIVClientStructs renderer/camera/CharaView definitions remain a research reference, not proof that a public layout matches the installed game.

## Research boundaries

Reflex 2 Frame Warp is not treated as a public drop-in replacement. Neural Texture Compression's inference-on-sample path needs neural texture assets and material shader integration; inference-on-load returns ordinary GPU textures and does not provide the same resident-memory saving. Neither is advertised as an implemented addon toggle.

Foveating NR's model grid is distinct from foveating the game's rasterization/SR projection. Do not warp only DLSS color while leaving guides/projection/history uniform. Any foveated DLSS experiment needs its own correctness and motion/disocclusion validation before it can replace ordinary SR.

## Primary references

- https://github.com/crosire/reshade/blob/main/include/reshade_events.hpp
- https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideManualHooking.md
- https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideReflex.md
- https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md
- https://github.com/intel/xess/blob/main/doc/xess_fg_developer_guide_english.md
- https://github.com/aers/FFXIVClientStructs/blob/main/FFXIVClientStructs/FFXIV/Client/Graphics/Render/RenderTargetManager.cs
- https://github.com/aers/FFXIVClientStructs/blob/main/FFXIVClientStructs/FFXIV/Client/UI/Misc/CharaView.cs
- https://developer.nvidia.com/performance-rendering-tools/reflex
- https://github.com/NVIDIA-RTX/RTXNTC/blob/main/docs/integration/InferenceOnSample.md
- https://github.com/maximedgoulet/FoveatedDLSS5/blob/main/src/addons/dlss5/FOVEATION.md
