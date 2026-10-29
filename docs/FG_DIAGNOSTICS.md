# Always-on frame-generation recordings

Build 0.4.10 adds paired scopes for the native Framework body, renderer update,
ReShade Present callback, private output Present, native DLSS feature release,
NR shutdown, and output/presenter destruction. ReShade effect work has its own
begin/end markers. These distinguish time outside vendor Present from time
inside it. The analyzer reports inter-Present intervals independently of
Present duration, named phase distributions, and unfinished scopes.

`session.end` records completed logger shutdown. A 0.4.10 recording without it
is incomplete even when its last Present finished. Older 0.4.9 recordings did
not have this marker and cannot certify orderly shutdown. NGX messages use
`sdk.ngx`; the callback belongs to the pinned host, so unloading Streamline
cannot leave a model-cache callback pointing into an unmapped plugin.

Every addon session records `XIVRender/logs/fg-<UTC>-<PID>.jsonl`. The overlay shows the actual path and any writer error. No diagnostic toggle or manual ETW start is required. Existing `XIVRender.log`, `ReShade.log` and vendor `runtime/sl.log` remain useful, but `sl.log` can be overwritten during a provider restart; the JSONL recording preserves its callback messages across those restarts.

The recording contains settings requests (including DLSS/HDR/NR context), adapter identity, module paths/PE timestamps and sizes, native captures/input rejection, each real Present and latency sleep, actual provider output counts, slow CPU stages, SDK messages, asynchronous DXGI errors, and handoff/SDK destruction/hook restoration events. A later normal status never replaces an earlier warning. Callback threads only append bounded in-memory records; a separate writer performs disk I/O and flushes roughly every 100 ms. A crash may lose that final unflushed interval. Queue overflow produces `records_dropped`; writer failure is visible in the overlay. Neither case is treated as a clean recording.

Each file rotates at approximately 32 MiB; the current file and `.1` through `.7` retain approximately 256 MiB per session. Older sessions are not automatically removed. Messages are bounded to 1,535 bytes and explicitly marked when truncated; invalid or split UTF-8 is replaced so the recording remains parseable. This is bounded diagnostic capture, not an unlimited or lossless recorder.

Run with Python 3:

```powershell
python tools/analyze_fg_recording.py logs/fg-<session>.jsonl --output build/fg-analysis.json
```

Pass the unsuffixed current filename; the analyzer reads its rotations in chronological order. It reports warnings, missing call completions, parse/queue loss, per-generation Present/sleep distributions, and one-second submission/output-count windows. Lost, malformed or unfinished records produce exit code 2; warnings remain in the output even if all calls completed. Counts describe SDK/native submission, not physical monitor scanout. Long Present alone does not establish a cause. Compare source-input gaps, sleep timings, option changes, SDK warnings and handoff generations around the same QPC timestamp.

Common event fields:

| Event | a | b | c | d |
| --- | --- | --- | --- | --- |
| `settings` | requested provider | packed FG request | requested DXGI format | fixed DLSS resolution |
| `present.begin` | requested factor | sync interval | forwarded flags | copy already completed |
| `present.end` | CPU duration ms | HRESULT | requested factor | valid interpolation request |
| `sleep.end` | CPU duration ms | — | — | — |
| `retirement` | retirement duration ms | success | actual presented count | source frame time ms |
| `dlssg.options` | mode | generated count | dynamic target | SDK result |
| `dlssg.resources` | flags | guide width | guide height | dynamic processing width |
| `dlssg.state` | presented count | status bits | retirement fence value | maximum generated count |
| `module.unload` | FreeLibrary succeeded | still mapped | — | — |

Providers: 0 ordinary, 1 DLSS-G, 2 XeFG, 3 FSR FG. `settings.b` packs factor in bits 0–7, target in bits 8–23, enabled at 32, dynamic at 33, and independent latency at 34. Every detailed event carries sequence, QPC, thread, real-frame ID and presentation generation. Initialization events occur under the preceding generation until `handoff.committed`; use `handoff.begin` and the SDK/module name to identify the target.

For a reproduction, keep the full recording plus rotations, `XIVRender.log`, `ReShade.log`, the INI and vendor logs. Review them even if gameplay appeared normal. Do not infer success from a test exit code or from the absence of visible stutter.
