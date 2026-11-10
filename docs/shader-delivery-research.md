# Shader delivery: Advanced Shader Delivery and Vulkan equivalents

Research only, 2026-11-07. No code was changed or built. Question: can Microsoft's Advanced Shader Delivery (ASD) cut first-run shader compile cost here?

**Verdict: no.** ASD is a Direct3D 12 feature, fed by a store, and its inputs are game-derived. A Vulkan equivalent already exists in this codebase. The remaining gap is cheap to close locally.

## 1. What ASD is

- A "Windows 11 Direct3D 12 technology" with three stages: create a State Object Database (SODB), test it by compiling a Precompiled Shader Database (PSDB) and registering it locally, deploy the SODB to a storefront ([overview](https://learn.microsoft.com/en-us/windows/win32/direct3d12/advanced-shader-delivery)).
- The SODB is a SQLite file holding serialized PSO and state-object descriptions plus their DXIL bytecode, recorded from `CreateGraphicsPipelineState`, `CreateStateObject` and similar calls ([spec](https://microsoft.github.io/DirectX-Specs/d3d/StateObjectDatabase.html)). Recording uses `d3dconfig.exe` capture or a programmatic API; capture "adds significant overhead" ([Agility SDK 1.618](https://devblogs.microsoft.com/directx/agility-sdk-1-618/)).
- The store compiles the SODB with each GPU vendor's compiler plugin (`D3D12StateObjectCompiler`) into a per-GPU PSDB and ships it with the download; Windows refreshes it after driver updates ([intro](https://devblogs.microsoft.com/directx/introducing-advanced-shader-delivery/)). The D3D12 runtime finds it inside the game process by executable path through the [registration API](https://microsoft.github.io/DirectX-Specs/d3d/ShaderCacheRegistrationAPI.html). A PSDB must match the adapter family and compiler ABI.
- Requirements: Windows 11 24H2 (build 26100.1000+) and Agility SDK 1.619.5+ ([adoption guide](https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12AdvancedShaderDelivery/readme.md)). Submission is through Xbox Partner Center with an application identity ([GDC 2026](https://devblogs.microsoft.com/directx/advanced-shader-delivery-whats-new-at-gdc-2026/)).
- Hardware per the [21 Oct 2026 post](https://devblogs.microsoft.com/directx/advanced-shader-delivery-is-available-now-across-hardware): AMD RDNA 1-4, Intel Arc B-series and Core Ultra 2/3, NVIDIA RTX, Qualcomm Snapdragon X2, each with a minimum driver. Delivery is through the Microsoft Store and Xbox PC app; Steam and Epic are "coming soon", undated.
- No Microsoft page I read mentions Vulkan.

## 2. Can it apply here?

**No. ASD needs D3D12.** The SODB records D3D12 create calls and DXIL; the PSDB is looked up by the D3D12 runtime. A Vulkan process never touches either.

Current flow (from the top comments of `runtime/gfx_shader.c` and `runtime/vk_pipeline.c`):
- An NVN program (control section, code, c1, translator answers) goes to `nx2navi_xlate.exe --serve` children and comes back as SPIR-V plus reflection, cached in memory and under `shader-cache/` (packed into `shader-cache.pack`). The key includes the translator identity, so any translator change invalidates every entry. `in/<input id>.in` files allow retranslation without guest memory.
- Play records `shader-answers.bin` and `shader-programs.bin`. `pipeline-keys.bin` records every built pipeline, shaders named by input id, independent of the translator.
- Pipelines use `VK_EXT_graphics_pipeline_library`: stage libraries, a fast link on a miss, an optimised pipeline built on a background thread. `pipeline-cache.bin` is dropped when vendor, device or cache UUID differ.
- Boot (`vk_pipeline_prewarm`) retranslates and rebuilds what the key file names. The installer runs `nx2navi.exe --prepare-shaders`, which warms the driver's disk cache.
- A comment in `runtime/boot.c` records a cold start (new translator or driver): 2,132 shaders translated and 1,579 pipelines built in 9.8 s. The guest waits at most `NX2NAVI_PREWARM_WAIT_MS` (default 3000), then the work continues in the background.
- Compute shaders are translated and built synchronously on the replay thread, because "a skipped dispatch can change what the game computes". Graphics misses are asynchronous and the draw is skipped. So wrong lighting early in a cold start more plausibly comes from skipped graphics draws than from compute. I did not reproduce or measure it.

**A D3D12 back end** would replace about 25,800 lines (28 `vk_*.c` files plus headers) using push descriptors, mesh shaders, pipeline libraries and dynamic state 3, and the translator would have to emit DXIL, not SPIR-V. My estimate is several person-months. It would still not reach ASD, because delivery needs a store submission (section 3). The local "compile a SODB, register the PSDB" test path runs the same vendor compiler on the player's machine, so it gains nothing over the existing prepare pass.

## 3. Content boundary

`AGENTS.md` forbids anything derived from game data in the repository. Shaders, SPIR-V, DXIL, SODBs, PSDBs and `pipeline-cache.bin` are all derived from game code. `check_content.py` also blocks `.pack` files.

- **Ruled out:** shipping or uploading any shader database, SODB, PSDB, SPIR-V or driver cache. ASD's deploy stage is exactly an upload to a store.
- **Still possible:** anything built on the player's machine from the player's own files. The installer's prepare step does this today, and everything stays in ignored folders.
- **Existing precedent:** the installer bundles a variant list (shader hash plus translator answers, no code), outside the repository. Keep any new shared data to that level. Whether a hash counts as "derived" is the owner's call.

## 4. Vulkan-side options

| Option | State here | Verdict |
|---|---|---|
| `VK_EXT_graphics_pipeline_library` | In use (`vk_device.c`, `vk_pipeline.c`, `NX2NAVI_GPL`) | Done |
| Driver disk cache | Used; a library built again on AMD takes 0.1 ms instead of 55 ms (`vk_pipeline.c`) | Done |
| Fossilize-style capture | `pipeline-keys.bin`, `in/*.in` and the answer files play that role, with the translator in front | Done. [Fossilize](https://github.com/ValveSoftware/Fossilize) replays on any device. [Steam pre-caching](https://www.phoronix.com/news/Steam-Vulkan-Shader-Pre-Cache) needs Steam and uploaded data: unusable |
| [`VK_KHR_pipeline_binary`](https://www.khronos.org/blog/bringing-explicit-pipeline-caching-control-to-vulkan) | Not used | Per-pipeline blobs with your own eviction, still per-driver. 52.8% of Windows devices expose it ([database](https://vulkan.gpuinfo.org/listextensions.php?platform=windows&extension=VK_KHR_pipeline_binary)); Mesa has it ([RADV](https://www.phoronix.com/news/RADV-Pipeline-Binary-Merged), [ANV](https://phoronix.com/news/Intel-ANV-Pipeline-Binary)). Per-vendor Windows support unconfirmed. Optional |
| [`VK_EXT_shader_object`](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_shader_object.html) | Not used | Skip: replaces pipelines, conflicts with the library design, binaries are device-bound |
| First-run "prepare shaders" pass | Installer runs it; boot prewarm capped at 3 s | The gap to close |

After a translator change the dominant cost is likely translation (CPU, translator children), which neither pipeline binaries nor ASD-style delivery can speed up. The driver compile only repeats for shaders whose SPIR-V changed. Measure the split first; the prewarm logs report both counts.

## 5. Recommendation

ASD is not feasible: D3D12-only, store-gated, built from game-derived data. The practical steps:

1. **Blocking "Preparing shaders" step on a cold cache (1-3 days).** When the translator identity, GPU or driver changes, run the existing prewarm to completion before the guest starts (the `NX2NAVI_PREWARM_WAIT_MS=0` behaviour), with progress shown. The `boot.c` figure is about 10 s, once. This removes the wrong-looking start.
2. **Re-run the library prepare pass after a driver change (2-4 days).** `lb_warm_prepared` and `--warm-shaders` exist, and the skip list already keys on vendor:device:driverVersion. Triggering the pass when `cache_load` drops a stale cache is the local counterpart of ASD's driver-update handling. I found no such trigger in `runtime/`; confirm before building.
3. **Measure, then decide on `VK_KHR_pipeline_binary` (1-2 weeks, low priority).** Prototype it only if the measurement points at the pipeline or driver cache. It could let libraries persist without the roughly 240 MB cache growth `vk_pipeline.c` avoids today.
