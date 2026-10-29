# Current test: 0.4.8 timing diagnostic

This is a measurement build, not a claimed fix. Rendering, frame-generation and latency behavior are unchanged from the failed 0.4.7 test. Native Tick, simulation/sleep, render/capture, D3D11 submission and private Present boundaries emit timing events only while the recorder is subscribed. Existing XeFG pacing statistics join the same trace. Ordinary gameplay does not write these events to disk.

The recorder uses signed Intel PresentMon, Windows Performance Recorder and NVIDIA's installed telemetry utility. It needs one Windows administrator approval, records no desktop video or keyboard input, uses a unique recording session and stops automatically after two minutes. CPU scheduling/driver stacks and graphics-queue events remain local in the project diagnostics folder.

Load the usual game scene and let the agent know when ready. Once recording starts:

1. Leave manual DLSS-G 6X running for about 30 seconds while moving the camera normally.
2. Switch to XeFG 4X for about 20 seconds to reproduce the slowdown.
3. Switch off both FG and NR, then leave the game running in the slowed state. Do not restart, close it or test FSR yet.

The agent will correlate the trace with the current logs and inspect the persistent state. The working OptiScaler fork's native D3D11 frame-retirement boundary is an investigation target; this build does not add or change that boundary.

The existing independent latency control, native FFXIV DLSS/DRS support, HDR, depth and NR settings remain as in 0.4.7. Current gameplay stutter and provider-switch slowdown are unresolved.