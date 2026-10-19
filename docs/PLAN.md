# gr-port — Planning document

Native Windows port of **Gravity Rush Remastered** (CUSA01113, Bluepoint Games, v01.00, SDK 2.00).
Strictly for personal use and technical study. No game assets or code are included in this
repository: the program reads the original files at runtime and never modifies them.

Legend: **[O]** observed (verified with this project's tools), **[I]** inferred (plausible, to be
confirmed), **[?]** unknown.

---

## 1. What is in the dump

### 1.1 Container
| Item | Value | |
|---|---|---|
| File | `Gravity_Rush_Remastered.pkg`, 9.87 GB, Content ID `EP9000-CUSA01113_00-GRAVITYRUSHHDEU1` | [O] |
| Type | "fake" PKG (fPKG): PFS decryptable with public debug keys, no account-bound data | [O] |
| Correct extraction | `tools\PkgTool-0.2.231\PkgTool.exe pkg_extract` (.NET Framework build) → `CUSA01113_v2\uroot` | [O] |
| **Corrupted** extraction | `PkgTool.Core` running on .NET 8: 64 KB PFSC blocks partially zeroed inside the `.psarc` files (eboot/prx unaffected) | [O] |

### 1.2 Executables (`uroot/`)
| File | Type | Notes | |
|---|---|---|---|
| `eboot.bin` | fSELF → `ET_SCE_DYNEXEC`, original name `launcher.elf` | 21.9 MB code (R-X), 13.8 MB data (RW), 0x140-byte TLS | [O] |
| `sce_module/libc.prx` | Sony libc, 2140 exports | satisfies most eboot imports through LLE | [O] |
| `sce_module/libSceFios2.prx` | Sony asynchronous I/O | built on libkernel (files, threads) | [O] |
| `libSceFace/Hand/HeadTracker/Smart/S3DConversion.prx` | PS4 camera / 3D | not needed for gameplay → phase 2 / never | [O]/[I] |
| `sce_sys/about/right.sprx` | system module | not loaded | [O] |

The eboot declares **42 required modules** and imports **681 symbols** from 44 libraries; it has
**302,608 RELA relocations** and 660 JMPREL. Our database names 559 of the 681 NIDs (82%).

### 1.3 Engine and middleware
| Component | Evidence | Implication | |
|---|---|---|---|
| Bluepoint engine (`Bluepoint_GravityDazeHD_PS4`, path `D:/dev/cp8/BP/Gravite/...`) | eboot strings | proprietary, no public SDK | [O] |
| **Havok 2014.1.0**, statically linked | `hk_2014.1.0` strings, `.hkx` tagfiles | physics runs **inside** the eboot: nothing to rewrite | [O] |
| **Lua 5.1.4 VM** inside the eboot, plain-text `.lua` scripts (UTF-8, Japanese comments) | `Lua 5.1.4` string, 1204 `.lua` files | game/event logic is readable and runs natively | [O] |
| GNM graphics (`libSceGnmDriver`, 50 functions) | imports | must be translated to Vulkan: the main workload | [O] |
| GCN shaders | compiled programs live in guest memory, loaded from the game data at runtime; captured with `--capture` (37 unique VS/PS programs on the title screen) | translated in phase 1.4 | [O] |

**Key architectural consequence:** the PS4 is x86-64 like the PC. There is no CPU recompilation:
the eboot code (engine, Havok, Lua, gravity logic) is loaded and executed as-is. The "port" means
providing everything *below* the game: Orbis kernel, system libraries, GPU, audio, input. Movement
and gravity control work unmodified once the platform is emulated well enough to run the game.

### 1.4 Assets (`uroot/arc/*.psarc`, PSARC v1.4 zlib, 21,897 files, ~31.7 GiB uncompressed)
The archives are split into PlayGo chunks (`ep_0_0`, `ep_1_7`, `ep_8_11`, `ep_12_30`, `ep_31_31`).
The game mounts them through Fios2 as `arc0`…`arc3`.

| Extension | Magic | Content (hypothesis) | Volume | PC conversion |
|---|---|---|---|---|
| `.gnf` | `GNF ` | Sony GNF textures, GCN tiled layout | 1694 files, ~22 GiB (chunk 31 is textures only) | GCN detiling → BCn/RGBA at runtime (VkImage); optional DDS export for debugging |
| `.gfx` | `GX64` | Bluepoint textures with a GNM T# descriptor (BCn blocks visible) [I] | 8641 files, 1.7 GB | as above |
| `.gcd` | `gcdL` | scene/level container (offset table, includes `ES02` skeletons) [I] | 189 files, 4.3 GB | read by the engine itself: no conversion |
| `.bppl` | `lpPB` | Bluepoint placement / pipeline [I] | few | none |
| `.hkx` | `1E0DB0CA CEFA11D0` | Havok tagfile (collision, skeletons) [O] | 2948 files | none (Havok is in the eboot) |
| `.lua` | text | game scripts [O] | 1204 files | none |
| `.sxd` `.sxd1` `.sxd2` | `SXDF` / `SXDS` | Sony sound banks (codec probably ATRAC9) [I] | ~725 MiB | decoding in the Ngs2 layer (phase 2) |
| `.mp4` | — | H.264 movies | 66 files, 2.4 GB | AvPlayer → Media Foundation (phase 2) |
| `.txb` | `txbL` | localized text [I] | 1136 files | none |
| `.anim` `.skel` `.manim` `.mskel` | `EA04`, `ES02` | animations / skeletons (Vita-derived format) [I] | — | none |
| `.evb` `.evs` `.ndg` | `FBKK`, … | events, AI node graphs [I] | — | none |
| `.ttf` `.otf` | — | fonts | 15 files | rasterized by our libSceFont (stb_truetype) |

The engine reads its own formats. The port only has to serve the original bytes through a virtual
filesystem (`/app0` → `uroot/`). The only mandatory conversion happens on the GPU side: tiled
textures and GCN shaders.

---

## 2. Port architecture

```
 ┌────────────────────────────────────────────────────────────────────────┐
 │ eboot.bin (launcher.elf) — Bluepoint engine, Havok, Lua, gameplay       │  guest x86-64 code,
 │ libc.prx, libSceFios2.prx (LLE: Sony modules shipped with the game)     │  executed natively
 ├──────────────────────────── imports by NID ────────────────────────────┤
 │ SysV → MS x64 thunks (loader/hle.cpp)                                   │
 ├────────────────────────────────────────────────────────────────────────┤
 │ Orbis HLE:  libkernel │ VideoOut │ GnmDriver │ Pad │ AudioOut/Ngs2 │ …   │  host C++ code
 ├────────────────────────────────────────────────────────────────────────┤
 │ Win32 (threads, memory, files) │  Vulkan  │  XInput / keyboard │ WASAPI │
 └────────────────────────────────────────────────────────────────────────┘
```

Repository components:
| Directory | Role | Status |
|---|---|---|
| `common/` | inflate, SHA-1, PSARC, SELF→ELF, Orbis ELF parser, NID | done (phase 0) |
| `tools/` | `psarc_tool`, `self2elf`, `nid_inventory`, `gr_disasm` (Capstone; PLT/GOT import names, RELATIVE pointers, `--xref`) | done |
| `loader/linker.*` | module mapping, relocations, import binding (LLE > HLE > stub), optional LLE call tracing | done |
| `loader/hle.*` | HLE registry, ABI thunks (stack realignment, stack args, xmm0-3), logging stubs | done |
| `loader/tls.*`, `loader/vmem.*`, `loader/guest.*` | `fs:` patching, static TLS, guest memory below 1 TiB, host→guest calls, guest threads, crash handler (guest + PDB host symbols), trace, thread dump | done (phase 1.1) |
| `loader/kernel/` | libkernel HLE: memory, threads/mutex/cond/rwlock, semaphores, event flags, equeue, files/VFS, time, internal libc | done for the boot path |
| `loader/kernel/` (services) | Pad (XInput, keyboard, `--input` scripts), AvPlayer (black frames), Ngs2/AudioOut (timed, silent), Fiber, Font (stb_truetype), SaveData + dialogs, PlayGo, DiscMap, offline trophies | done for the boot path; real audio and movies: phase 2 |
| `loader/gpu/` | GPU thread with PM4 parser (EOP/RELEASE_MEM, WRITE_DATA, WAIT_REG_MEM, DMA, IB, register shadowing), compute queues, `libSceGnmDriver` HLE, VideoOut with a 60 Hz vblank, shader/draw capture | done (1.3) |
| `loader/gpu/shader_recompiler.*` | GCN (CI) → GLSL → SPIR-V (glslang); host-side scalar pre-execution for constant buffers and descriptors | done for the title screen (1.4) |
| `loader/gpu/renderer.*`, `vk_device.*` | draw → Vulkan: render-target/depth cache keyed by guest address, textures (render-target copies or detiled uploads), samplers, pipeline cache, flip → output image, PNG frame dumps | done for the title screen (1.4) |
| `loader/vk_window.*` | Win32 window + Vulkan swapchain; presents the renderer's output, or uses its own device on the GPU that drives the monitor (frames passed through host memory) | done |

---

## 3. Conversion tables

### 3.1 Calling conventions and TLS
| PS4 | Windows | Notes |
|---|---|---|
| System V AMD64 ABI (rdi, rsi, rdx, rcx, r8, r9, xmm0-7) | MS x64 (rcx, rdx, r8, r9 + shadow space) | generated thunks: 6 integer registers, 3 stack args, xmm0-3, return address; the stack is realigned (guest code does not always call 16-byte aligned) |
| TLS via `fs:[0]` | the FS base cannot be set from user mode on Windows x64 | `mov reg, fs:[0]` rewritten in place into `mov reg, gs:[TlsSlots + 8*slot]` (same length, zero cost) |
| Host→guest callbacks (thread entries, SDK callbacks) | — | MS→SysV stub preserving rdi/rsi/xmm6-15 |

### 3.2 GNM/GNMX → Vulkan
| GNM (actual eboot imports) | Vulkan | Complexity |
|---|---|---|
| `sceGnmSubmitCommandBuffers`, `sceGnmSubmitAndFlipCommandBuffers` | PM4 parser (type-3 packets) → Vulkan command buffers, `vkQueueSubmit` | high |
| `SET_CONTEXT_REG` / `SET_SH_REG` / `SET_UCONFIG_REG` | pipeline state → `VkPipeline` (cached by state + shader hash) | high |
| `sceGnmSetVsShader`, `SetPsShader`, `SetCsShader`, `SetEs/Gs/Hs/LsShader` | **GCN ISA → SPIR-V recompiler** | very high: the core of the project |
| `sceGnmDrawIndex*`, `DrawIndexAuto`, `DispatchDirect` | `vkCmdDrawIndexed` / `vkCmdDraw` / `vkCmdDispatch` | medium |
| V#/T#/S# descriptors in guest memory | buffers / `VkImage` + samplers, with GCN detiling | high |
| CB/DB render targets | `VkImage` attachments + resolves/copies | high |
| EOP events, label writes, `sceGnmAddEqEvent` | label writes into guest memory + equeue events | done (1.3) |
| `sceGnmSubmitDone`, `sceGnmFlushGarlic` | no-op | done |
| unified CPU/GPU memory | guest page tracking (write protection) to invalidate buffers/textures | high |
| `sceVideoOutOpen/RegisterBuffers/SubmitFlip/AddFlipEvent` | vblank thread + equeue; flips are queued behind GPU work and copy the render target at the display buffer's address to the presenter | done (1.4) |

### 3.3 Orbis (libkernel and services) → Win32
| Orbis | Win32 / host | Status |
|---|---|---|
| `scePthread*`, `pthread_*` | Win32 threads + entry thunks, SRWLOCK-based mutexes with PS4 semantics, `CONDITION_VARIABLE`, host TLS | done |
| `sceKernelAllocateDirectMemory`, `MapDirectMemory`, `MapFlexibleMemory`, `Mprotect` | `VirtualAlloc` below 1 TiB (direct memory aliasing still missing) | mostly done |
| `sceKernelOpen/Read/Lseek/Stat/Getdents` | `CreateFileW` & co. on a VFS: `/app0` → `uroot` (read-only), `/savedata0` → mounted save dir | done |
| `sceKernelCreateEqueue/WaitEqueue`, event flags, semaphores | condition-variable based objects | done |
| `sceKernelGetProcessTime*`, `ReadTsc`, `Usleep` | `QueryPerformanceCounter`, `Sleep` | done |
| `libSceLibcInternal` (imports of the bundled prx) | host CRT; printf family decodes the SysV `va_list` | done |
| `libc`, `libSceFios2` | **LLE**: the game's own `.prx` modules | done |
| `scePad*` | XInput / keyboard / scripted input, DualShock 4 data layout | done |
| `sceUserService`, `sceSystemService`, `sceSysmodule`, `scePlayGo`, `sceDiscMap` | constants (English language, everything installed) | done |
| `sceAudioOut`, `sceNgs2` | timed output + voice state machine (silent) → WASAPI + decoding in phase 2 | partial |
| `sceAvPlayer` | a few black frames per movie → Media Foundation in phase 2 | partial |
| `sceSaveData*`, dialogs | directory per save + auto-confirmed dialogs | done |
| `sceFont*` | stb_truetype | done |
| NP, Trophy, Http, Net, WebApi | offline stubs | stubs |

### 3.4 Asset formats → PC
See §1.4. Summary: **no offline conversion**. At runtime only GNF/GX64 textures (GCN detiling +
upload) and GCN shaders (→ SPIR-V, with a disk cache) are converted.

---

## 4. Complexity estimate per subsystem

Estimates for one experienced developer, full time. Indicative only: the real-world reference is
shadPS4, developed by a team over several years.

| # | Subsystem | Complexity | Estimate | Phase |
|---|---|---|---|---|
| 0 | Tools, loader, linker, thunks, Vulkan window | — | **done** | 0 |
| 1.1 | Guest execution: `fs:` TLS patching, SysV stacks, module init, main thread | high | **done** | 1 |
| 1.2 | libkernel: memory, threads, sync, files/VFS, equeue | high | **done for the boot path** | 1 |
| 1.3 | VideoOut + minimal PM4 (flip, EOP labels) | medium | **done** | 1 |
| 1.4 | GCN → SPIR-V recompiler (VS/PS done; CS, ES/GS/HS/LS to do) | very high | **title screen done**; in-game shaders next | 1 |
| 1.5 | Pipeline state, render targets, texture detiling, memory tracking | very high | **basics done** (1D detiling, RT cache); 2D macro tiling, compute, performance to do | 1 |
| 1.6 | Pad → XInput / keyboard | medium | **done** | 1 |
| 2.1 | Audio (AudioOut, Ngs2, Audio3d, ATRAC9) | high | 1–2 months | 2 |
| 2.2 | Movies (AvPlayer) | medium | 2–3 weeks | 2 |
| 2.3 | SaveData + dialogs | medium | **done** (dialogs auto-confirm) | 2 |
| 2.4 | Neo mode, resolution/framerate, shader cache | medium | — | 2 |
| 2.5 | NP / trophies / network | low (stubs) | — | 2 / never |

---

## 5. Incremental roadmap (every step has a verification criterion)

**Phase 0: done.** PSARC readable (21,897 files), correct SELF→ELF, NIDs verified against known
values (`__stack_chk_fail` = `Ou3iL1abvng`), 8 modules loaded, 307,000 relocations applied with no
unsupported types, thunk self-test passed, Vulkan window at 60 Hz.

**Phase 1: goal "game window, level loading, movement and gravity control".**
1. *Guest execution.* **Done.** `gr_port --run` records the real boot sequence in
   `gr_port_trace.txt`: `libSceFios2` init → `libc` init (game heap: 1 GiB of direct memory +
   `sceLibcMspaceCreate`) → eboot entry → game and Fios2 threads → PSARC access through Fios2 →
   `sceVideoOutRegisterBuffers` → `sceGnmSubmitCommandBuffers`. Technical findings:
   - 2752 `fs:[0]` reads rewritten in place into `gs:[TlsSlots+8*slot]`;
   - guest memory must stay below `0xFC_0000_0000`: libc rejects heaps above `0x3E<<34`
     (see `loader/vmem.*`);
   - the game replaces libc's `malloc` with its own allocator (table in `SceLibcParam`).
2. *libkernel.* **Done for the boot path.** Still missing: direct memory aliasing, thread TLS
   destructors, `sceKernelGetModuleInfoFromAddr`, `sysctl`.
3. *VideoOut / minimal PM4.* **Done.** The frame loop runs at 60 Hz: ~60 submits/s, ~600 draws per
   frame (counted only), no invalid PM4 packets or faults.
   **Complete logical boot, verified with scripted input and the game's own log output:**
   - first boot (no save): `TITLE: eula` → `savedata check` → `TITLE: first` → opening movie →
     `cutA` demo → 20 taps on the apple → `cutC` demo → `Select NewGame` → movie → **Episode 1**
     (`currnetmission ep01_a`, area "cc", `Fade In`, comic dialogue);
   - with a save present: `TITLE: title` → **title menu** (New Game / Continue / Gallery / Staff)
     responding to navigation → New Game → opening → Episode 1.
   Problems solved along the way: `pthread_create_name_np` for the engine's worker pool; mutexes with
   PS4 semantics (errorcheck/recursive) on SRWLOCK; Ngs2 voices with state and advancing sample
   counters (the `cutC` demo is synchronized with the audio stream); AvPlayer delivering at least one
   frame (video textures are created on the first frame); stack-realigning thunks; `float`
   arguments (xmm0-3) in thunks.
   **Open:** the opening comic dialogue of Episode 1 (`KaiwaManager`) does not advance with Cross;
   to be investigated (probably needs a GPU result or a different input).
4. *Shaders + draws.* **Title screen done** (verified with `--dump-frames` PNGs and in the window):
   artwork, logo, menu text, copyright line. How it works:
   - **Recompiler** (`loader/gpu/shader_recompiler.cpp`): one Vulkan invocation emulates one GCN
     lane. SGPRs/VGPRs become local arrays, EXEC/VCC/64-bit masks are reduced to this lane's bit and
     every VALU write is guarded by EXEC. Branches become a `switch` dispatcher inside a loop. The
     fetch subroutine is inlined at `s_swappc_b64`. Scalar memory loads (`s_load_*`,
     `s_buffer_load_*`) are pre-executed on the host before each draw and passed in a storage buffer
     (`sdata`) together with the user SGPRs. Each image/buffer instruction gets a descriptor binding;
     instructions reading the same descriptor registers with no write in between share one.
     Vertex fetch decodes the V# data/number format in the shader. PS inputs follow
     `SPI_PS_INPUT_ADDR` and `SPI_PS_INPUT_CNTL_n`; PS colour outputs are swizzled to the CB
     component order (`COMP_SWAP`), so render-target memory layout matches the PS4.
   - **Renderer** (`loader/gpu/renderer.cpp`): one Vulkan draw per GNM draw with dynamic
     rendering. Render targets and depth buffers are host images keyed by guest address. A T# that
     points at a render target gets a copy of it (depth is copied through a buffer into an R32F
     image); other textures are uploaded from guest memory, with 1D micro-tile detiling
     (tile mode 13) and a content hash checked once per submission. Depth/stencil clears via
     `DB_RENDER_CONTROL` are applied directly. Each submission is one command buffer, submitted and
     waited for at its end. A flip copies the render target at the display buffer's address to the
     output image.
   - Title frame: 75 draws, ~20 render targets (HDR scene, bloom chain, tonemap, UI), 86 shader
     variants, 0 translation failures.
   Bugs found along the way: `sceVideoOutSubmitFlip` must go through the GPU queue (the flip has to
   follow the submitted work); `SceFontRenderOutput` is 40 bytes with the glyph image position at
   +0x18 and its size at +0x20 (found by disassembling the game's caller: the menu text was laid
   out from the wrong fields); the window's GPU (iGPU driving the monitor) may differ from the
   rendering GPU; the crash handler must leave faults inside driver DLLs to their own SEH handlers;
   the title intro samples the depth buffer's **stencil plane** (at `DB_STENCIL_READ_BASE`) as an R8
   texture, so stencil addresses are mapped to the stencil aspect of the host depth image; the
   presenter needs a barrier between clearing the swapchain image and blitting the frame into it
   (without it RDNA3 showed tiles of the clear colour over the picture).
   **Episode 1 renders** (opening demo, comic dialogues, the Hekseville courtyard in gameplay; the
   comic dialogue that used to hang now advances). Added for it:
   - compute dispatches on the graphics ring: LDS (`ds_read/write(2)`, atomics), wave64 subgroups
     required (the luminance reduction relies on lockstep instead of `s_barrier`), `image_store`
     into render-target images, buffer stores written back to guest memory after a synchronous
     submit; VCCZ/EXECZ branches evaluated over the subgroup;
   - HTILE fast clears: a compute shader that rewrites a depth buffer's HTILE
     (`DB_HTILE_DATA_BASE`) clears the host depth image to `DB_DEPTH_CLEAR` — without it the main
     depth stayed at 0 and the whole scene failed the depth test;
   - null T# (all zero) bound as a 1x1 black texture instead of dropping the draw (the main city
     mesh uses one);
   - performance: draw recording went from ~1.6 ms to ~25 µs (VirtualQuery results cached; index
     data no longer read back from write-combined memory; vertex-buffer copies bounded by the
     highest index); Vulkan pipeline cache saved to `build\bin\cache\pipelines.bin`;
   - debugging: `GR_DUMP_RTS=<submission>` (every render target as PNG), `GR_DRAW_TRACE=<submission>`
     (one line per draw with the renderer's decision), compute programs in `--capture`.
   **Known problems seen by the user in a play-through (windowed):** in the opening cutscene the
   world geometry "explodes" (stretched/scattered vertices) while the camera approaches the apple;
   when the apple falls the ground is black; after the dialogue Kat moves (controls work) but is
   invisible; no audio (phase 2); unstable frame rate.
   **Investigation of the invisible Kat (2026-10-16, evening):**
   - Skinned vertex shaders (bone palette fetched with indices `v20..v23`, e.g. VS `0x1049a6e100`,
     `0x1049a6c300`, `0x104e806900`) are used only during the opening (submissions ~2880-6200, the
     apple tree on the floating island is skinned). From the courtyard scene on ("Where...am I?")
     **no skinned draw is submitted at all**: not Kat, not the cat. So the game itself skips the
     characters. It is not a renderer skip. Kat's data is loaded: `gfx1/kit01_base.gfx`,
     `tex1/bp_kit01_body*.gfx` and the `eaf/kit01_*` animations are all read.
   - Every frame two textures are unreadable: T# `0x4040000000` (2048x2048, 32-bit) and
     `0xffffff0000` (1x1). These are the T#s with high bits set (the dw1 low byte 0x40...). They
     are the next lead: the address decoding may be wrong, or the game may expect memory there
     (e.g. a GPU-written buffer the CPU reads back to decide visibility).
   - Not the cause: missing occlusion queries/predication (the game issues no `OCCLUSION_QUERY`,
     `SET_PREDICATION` or indirect draws), and vertex-buffer truncation. Truncation was a real bug
     and is fixed: buffers indexed by `instance id * 8 + k` were cut to the vertex range. Now only
     buffers fetched with the untouched vertex index (`ResourceSlot::vertex_indexed`) are bounded.
   - The 112 draws of VS `0x104e848e00` (a `dot(n, v)` mask pass) fail the depth test, which
     looks legitimate (hidden geometry).
   - New debug switches: `GR_DRAW_TRACE` now also runs an occlusion query per draw (`drawquery N:
     samples`) and logs every buffer binding (V# word 3, base, stride, records, bounding box and
     first values); `GR_FIND_VS=<hex>` logs the submissions that use a vertex shader;
     `GR_DEBUG_NODEPTH=<hex VS>` disables the depth test for one VS; `GR_DEBUG_NOKILL=<hex PS>`
     disables pixel kill for one PS; `GR_DUMP_GLSL=1|<hex>` writes the generated GLSL to
     `build\bin\glsl`. Unreadable textures are logged in the trace.
   - About the reported "black ground when the apple falls": around flips 2340-2380 the camera
     follows the apple through a dark void with flying grass. That may simply be the scene, so it
     needs checking against the real game before it is called a bug.
   **2026-10-17:** the T# base address is 38 bits (`dw1 & 0x3F`; bits 38-39 are MTYPE_L2), as
   shadPS4's `AmdGpu::Image` shows, and the V# base is 40 bits (`dw1 & 0xFF`). The `0x40...`
   "high bits" were MTYPE bits, now fixed. `0xffffff0000` is our own 1x1 null texture (harmless).
   Textures that cannot be resolved now bind the black texture instead of dropping the draw.
   RenderDoc is integrated (`GR_RENDERDOC`). shadPS4's source (`../shadPS4-src`, GPL-2.0, used
   as a reference only) has the 2D macro-tiled detiling in
   `src/video_core/amdgpu/tiling.cpp` and `texture_cache/tile_manager.cpp`.
   **KAT IS VISIBLE (2026-10-18): the legacy geometry pipeline (ES → GS → copy VS) is emulated.**
   Characters (Kat, the cat, the pigeons) are drawn with `VGT_SHADER_STAGES_EN = 0xb0`: an export
   shader (ES, the skinning) writes the ESGS ring, a geometry shader (GS, up to 8 vertices per
   triangle, with cuts) writes the GSVS ring, and the VS is only the copy shader that reads the GSVS
   ring. We ran just the copy shader over a ring nobody filled, so every character draw had zero
   area. How it was found: guest breakpoints showed Kat passing the game's culling and reaching its
   draw queue (0x1c, material type 0x2f, executor eboot+0xed2f10, draw eboot+0x142c0); the draws
   reached the GPU but read an all-zero 4 MiB buffer that neither the CPU, DMA nor any captured shader
   wrote; `sceGnmSetEsShader` / `sceGnmSetGsShader` were being called (7241 times each).
   Emulation (renderer `RunEsGs`, recompiler `ShaderKey::gs_mode`):
   - the ES runs as a compute shader, one invocation per index of the draw (lane i % 64 of wave
     i / 64, ES2GS_OFFSET = wave × 64 × VGT_ESGS_RING_ITEMSIZE × 4); the GS runs as a compute shader,
     one invocation per triangle, with v0/v1/v3 = the ESGS dword offsets of its three vertices,
     v2 = primitive id, GS2VS_OFFSET / wave id in the SGPRs after the user data; `s_sendmsg` GS
     emit / cut are counted per invocation and stored (count | cut mask << 8) in an aux buffer;
   - buffer accesses now implement GCN swizzled addressing (V# swizzle_enable, element size, index
     stride, ADD_TID) in `buf_addr`, so the ring layouts are exactly the hardware ones;
   - the rings are two dedicated GPU buffers (ESGS, GSVS, 64 MB each), bound by exact V# base match
     for the current draw (only shaders touch them). The game's GSVS ring sits 4 MB after the ESGS
     ring; an earlier version bound rings by address range, so the GSVS landed inside the ESGS
     buffer and large draws were clipped. Fixing it brought back the texture coordinates of the
     characters: Kat's dress patterns and forearm ornaments now render (2026-10-18);
   - the draw becomes the copy VS over a triangle list: instance = GS invocation, vertex = corner
     of triangle j of its strip; v0 = the GSVS offset of strip vertex e; triangles past the emitted
     count or across a cut get a zero position.
   - `sceGnmSetGsShader` now also writes VGT_STRMOUT_CONFIG / VGT_GS_OUT_PRIM_TYPE /
     VGT_GS_INSTANCE_CNT, and `sceGnmSetEsShader` applies the shader modifier (as shadPS4 does).
   Remaining for the geometry pipeline: only triangle-list input is handled; GS instancing, streams
   other than 0 and output primitive types other than strips are not; tessellation (LS/HS) is not
   emulated.
   Open: Dusty (the cat) still looks wrong (grey-blue blotches instead of a black body with stars
   and a light outline). Draw: VS 0x24e617d00 / PS 0x24e622a00, ES 0x24e612d00, GS 0x24e617f00;
   diffuse 256x256 BC3 (alpha-masked, grey-blue clouds in RGB) and a 256x256 BC1 starfield sampled
   at param4 = a 2x2 UV transform of the mesh UV (ES, cbuffer dwords 0x1c-0x2d). The PS adds
   star * diffuse.a * constant to diffuse * lighting; the body colour must come from the material
   constants or a later pass.
   **Mip levels (2026-10-19):** micro-tiled textures (1D thin, tilings 5/9/13) now upload their
   whole mip chain (T# last_level), laid out as in shadPS4 `ComputeImageSize`: level i is
   `pitch >> i` (power-of-two padded with pow2pad, in blocks for BC), padded to 8x8 elements and
   widened until a slice is a multiple of 256 bytes, levels stored one after another with every
   slice inside each level (`MicroMipChain`). Distant ground is visibly less aliased. Linear and
   macro-tiled textures still get mip 0 only; the square pow2 BC tables (Pow2Bcn64/128ImageTable)
   are not used yet, so check those textures if something looks off.
   This did **not** fix the cat: its UVs are right (param0 = diffuse UV, param4 = star UV at 2x),
   and the diffuse body is grey-blue with high alpha, to which the PS adds the stars. The black
   body seen on PS4 must come from the material constants (s[12:15] colour at cbuffer dword 0x14,
   star strength at 0x11) or the lighting terms; read those constants for this draw next.
   **Shadow compares (2026-10-19):** `image_sample_c` used to compare `zref <= filtered depth`
   whatever the sampler said. It now reads DEPTH_COMPARE_FUNC from sampler word 0 (bits 12-14),
   clamps zref to [0,1] and, for 2D / 2D-array lookups, compares the four texels of a
   `textureGather` and blends them bilinearly (2x2 PCF, as the hardware filters compare results).
   Characters now get proper self-shadowing (Kat's hair and cape). Not verified against a PS4
   frame yet: if scenes look too dark, check the compare function the game uses.
   The cat is unchanged by mips and shadows. RT dumps show the pattern is already in the main colour
   target right after the cat's own draw (not a post-process): a black body crossed by a light
   grey web. Next suspects: the PS's light loop (tbuffer light list at s[8:11]), the cbuffer
   material constants, or the GS output normals (param1/param2) the PS uses for lighting.
   The missing movie between the apple and gameplay is `mp4/ep00-01c.mp4` (AvPlayer is a stub).
   **Kat investigation, 2026-10-17 (afternoon), state when stopped:**
   - Auto-exposure fixed: the game reads the 1x1 average-luminance render target on the CPU
     (eboot+0x27358f). Tiny colour targets (<= 64 px) are now copied back to guest memory after
     every submission (`ReadbackTinyTargets`). The courtyard is much brighter.
   - Kat is NOT culled. The per-object cull job (eboot+0x26d010, list at ctx+0x2a68, AABB as
     centre/extent at item+0xa0/+0xb0, visible flag at item+0x1a2) marks items 0-2, at Kat's
     position (332,-102,-55), as visible, and some pigeons too. The draw-model job
     (eboot+0x26dd30) then computes skinned bone palettes (eboot+0x3c500, about 6 per frame in
     gameplay) and calls the main-pass submit (eboot+0x269630, a per-material-type handler table
     at 0x1ef8f88). So skinned meshes reach the game's draw lists, but no skinned draw reaches the
     command buffer. **Next:** follow the material handler from 0x269630 to where the draw lists
     are executed (draw path seen in the opening: 0x140e3 -> 0x19060 -> vcall [+0x38] ->
     0x19360 -> SetVs 0x4715d0) and find why the entries are dropped.
   - Debug markers now reach the trace (`marker push Stencil`, `Screen`): gameplay frames have
     only these two named groups around the draws.
   - Not causes: LOD distances (CharacterLodDistance = 30, 50, 200, 400), frame delta time, CE/CCB,
     occlusion queries (EVENT_WRITE is only type 0x2c).
   - Tools added: Ghidra headless project `../tools/ghidra_proj` with scripts
     `../tools/ghidra_scripts/Decomp.java` (decompile the functions containing eboot offsets) and
     `StrRefs.java` (string -> references -> decompiled callers); `GR_BP=<off>[:<reg>:<delta>],...`
     (int3 hit counters on eboot code, optional register capture + float dump),
     `GR_PEEK=<off>,...` (eboot globals), `GR_CULLDUMP=1` with `GR_BP=26d010:rsi:0` (cull list
     dump), `GR_WATCH_RTS=1` (logs the first CPU access to render-target memory),
     `GR_DEBUG_SETVS=<hex>` (backtrace of who binds a VS), `GuestStackScan()`.
   - One run (out of about 10) lost the Vulkan device (fence timeout) near submission 2921: an
     intermittent GPU hang, not investigated yet.
   - GR2fork (`../GR2fork`, shadPS4 fork for Gravity Rush 2) was studied. Its relevant part is the
     GPU-to-CPU readback work (render targets and buffers read by the CPU).
   **Black rocks / trunk / ground: FIXED (2026-10-17).** Two recompiler bugs, both general:
   - *48-bit SMRD addresses.* `s_load_dword*` took the full 64-bit SGPR pair as the address. When a
     shader uses a V# as a pointer (`s_load_dwordx8 s36, s[12:13], 0x20` with `s[12:15]` = V#), the
     stride in the high dword (`0x00100002`) made the address invalid, so the table read as zeros
     and the four textures were null. The GPU uses 48-bit addresses: the high dword is now masked
     with `0xFFFF` as for `s_buffer_load`. This fixed the stone and bark textures and the black
     ground where the apple lands.
   - *Pre-execution across branches.* The scalar pre-executor ran instructions in program order,
     so a register reused on a skipped path (`s_mov_b32 s4, 3.0` in the `if` branch) leaked into a
     T# used on the `else` path. It now does a second pass that follows the scalar control flow
     (SCC branches decided, VCCZ/EXECZ taken as falling through), and resources and SMRD results
     seen on that path win. The program-order pass is kept for code the walk does not reach.
   - The constant engine (CCB: `WRITE/LOAD/DUMP_CONST_RAM`, CE/DE counters) is now emulated as in
     shadPS4, although Gravity Rush submits no CCBs (0 packets).
   **Earlier analysis of the black rocks (kept for reference):** shadPS4 renders the
   island with stone and bark textures. In ours, rocks, hills and the trunk come out black. Their
   pixel shader (e.g. PS `...e900`) takes the shadow-cascade array (4 x 2048x2048 R32F, sampled with
   `image_sample_c ... da`) from its descriptor table at `s[2:3]+0`. The good T# elsewhere is
   `0244f600 1c400000 01ffc7ff d4000924 00ffe003 00006000`. The one in the rock table is identical
   except dw0 = `0x40400000` = **3.0f**, so its address points at the unmapped `0x4040000000`.
   With the image filled with 1.0 instead of 0 (`GR_DEBUG_UNREADABLE_ONE`) the rocks turn white,
   so this one dword is the whole bug. Next: find which guest instruction writes that dword (a
   watchpoint on the table, or a scan of the eboot code that fills the material tables), and
   check whether one of our HLE results feeds it.
   Also changed: direct memory mapped without a hint is now placed from `0x2'0000'0000` up, like
   Orbis and shadPS4 (`DEFAULT_MAPPING_BASE`), instead of `0x10'0000'0000`.
   **Next:** find which CPU-side check hides the
   characters (trace the game's draw-list code for `kit01`); 2D macro-tiled textures, rect lists,
   compute-queue dispatches, performance.
5. *Input.* **Mostly done** (full pad); visual verification pending.
6. *Gameplay.* Verification: Kat moves and gravity control works. The logic is native guest code,
   so nothing specific is needed beyond the previous steps.

**Phase 2:** real audio (WASAPI + SXD bank decoding), movies (Media Foundation), system dialogs with
a UI, performance, graphics options. Saves and fonts already work.

**Phase 3: mod support.** Starts once the game is playable. `gr-port` is a translation layer: the
original eboot runs unmodified on top of our Orbis reimplementation and GNM→Vulkan translation.
Because every file, script, GPU command and code page passes through the loader, mods can hook in
at these points:
- *Assets:* a `mods/` folder whose loose files override the ones in the `.psarc` archives (the
  VFS already resolves every path), with a load order between mods.
- *Lua:* most mission, event and dialogue logic is plain-text Lua (`lua/game/...`), so it can be
  replaced or extended without touching code.
- *Code:* patches and hooks at eboot offsets, applied by the loader (the `GR_BP` int3 machinery is
  a first step), with a documented patch file format.
- *Graphics:* texture and shader replacement keyed by hash in the renderer, higher resolutions,
  unlocked frame rate, post-processing.

**Phase 4: reverse engineering (a challenge, after phase 3).** The goal is to see how far a real
port can go: move from "run the original binary" towards understanding and rebuilding the game's
code. Possible steps, each useful by itself:
1. Map the eboot in Ghidra (project already in `../tools/ghidra_proj`): name subsystems, classes,
   vtables and the Lua bindings (`setVisible`, `EntitySpawner:Spawn`, …). The RTTI/type names in
   the binary (Havok, `ActorGfxData`, `Gfx::*`) help.
2. Write C++ replacements for selected functions, hooked over the originals, verified against
   them (same inputs → same outputs).
3. Grow the replaced part subsystem by subsystem (renderer front-end first: it is the part we
   already know best), until the original code is only a shrinking fallback.
This phase is open-ended; how far it goes is part of the experiment.

**Testing infrastructure (2026-10-19).** Replaying the prologue for every test does not scale, so
gr-port has opt-in shortcuts built on an in-memory overlay of the game archives: `--skip-opening`,
`--warp MISSION` (start New Game at any main mission), `--mods DIR`, plus `--savedata`, `--dump-at`
and `--exit-after-dumps`. `tools/run_scenario.ps1` runs named scenarios (`tools/scenarios.json`)
and compares frames with reference captures. The original files are never modified, the default
command line runs the game as shipped, and the repository holds only search/replace rules, never game
code: see [MODDING_AND_TESTING.md](MODDING_AND_TESTING.md). Real saves (the game autosaves at the
start of each episode except `ep01_a`) are the next checkpoint mechanism; emulator-level save states
are not planned.

**Movies (2026-10-19).** `loader/kernel/avplayer.cpp` now decodes for real. The game passes a path
inside its archives (`mp4/ep00-01c.mp4`) plus file callbacks (SceAvPlayerFileReplacement at init
data +40: open, close, readOffset, size); the player thread reads the whole file through them,
opens it with an IMFSourceReader from memory (H.264 → NV12, AAC → PCM16) and fires READY. A host
decoder thread keeps two frames ahead in four guest NV12 buffers (allocateTexture, pitch aligned to
64, height to 16, crop offsets for the padding, as the real library and shadPS4 do);
GetVideoDataEx hands out a frame once the clock since Start reaches its timestamp. The intro (Japan
Studio, Bluepoint, title animation: `subtitlemovie_us.mp4`, 19 s) and the 161 s `ep00-01c` movie
after the apple play with their subtitles; CROSS skips them. Audio frames are decoded and handed
out by GetAudioData (the game's MovieAudioThread pushes them to sceAudioOut).

**Audio (2026-10-19).** sceAudioOut ports are mixed into one shared WASAPI stream
(`audio_out.cpp`: 48 kHz float stereo, per-port rings, resampling, 8-channel downmix, volume).
The game's sound engine ("sndx") renders through libSceNgs2, now implemented in `ngs2.cpp`:
- graph: sampler rack 0x1000 → submixer 0x2000.0 → mastering 0x3000.0, reverb 0x2001.0 → submixer;
  patches may target a rack handle (meaning its voice 0); matrices are levels[out * inputs + in];
- formats: 0x14 = PCM 24-bit LE (sound effects, mono/stereo), 0x40 = ATRAC9 (music, streamed in
  ~32 KB blocks from a ring at 0x2007b7980; config word 0xFE740DF0 = bytes FE 74 0D F0), decoded
  with LibAtrac9 (external/libatrac9, MIT). ATRAC9 frames sit in fixed-size superframes: skip the
  padding after each superframe or the decoder derails after a few frames;
- streams: the game sends Play, Pause, then Resume once the voice has pre-decoded (a paused voice
  must fill its codec cache, i.e. decodedDataSize > 0), and keeps appending blocks while watching
  numDecodedSamples / decodedDataSize; a voice that stops advancing is stopped by the game;
- 10 render buffers: 0 = mastering voice 0 (8 ch float, 256 frames), 1-4 mono, 5-8 stereo,
  9 = 8 ch; buffer i is written from mastering voice i;
- not modelled yet: envelopes, filters, reverb (silent), port delays, voice callbacks.
Courtyard: continuous music/ambience and effects in the mix (checked with GR_AUDIO_DUMP; levels
look low, around -44 dBFS RMS, to be compared with the real game).
Next session: with audio running the game's timing shifted, so `opening` (frame 3300), `movie`
(4300) and `ep03` (3400) now capture the same scenes at slightly different moments; refresh those
references (`run_scenario.ps1 opening movie ep03 -SaveReference`) and prefer stable frames. For
the cat, `GR_DEBUG_BLACK_TEX=24e622a00:7` (stars) or `:0` (diffuse) isolates the shader terms.

Working rule: if a subsystem blocks progress, close it with a logging stub (the mechanism exists)
and move on. `gr_port_report.txt` lists the functions that are still stubs, per library.

---

## 6. Risks and open questions
- **GPU workload.** The GCN recompiler is a project of its own. shadPS4 (GPL-2.0) is a useful
  reference; copying its code would make this project GPL-2.0, which is fine for personal use.
- **Memory layout.** Solved for the boot path: every guest allocation goes through
  `GuestVirtualAlloc` and stays below 1 TiB. Host objects handed to the guest as handles (threads,
  mutexes, `libSceLibcInternal` malloc) still live above it; no check has rejected them so far.
- **Variadic float arguments.** Thunks capture xmm0-3, but host `printf` emulation only sees
  floats through a `va_list`; variadic float arguments passed in registers print as 0.
- **Unnamed imports.** A few NIDs are still unnamed; more complete name lists (e.g. ps4libdoc dumps)
  can be appended to `data/orbis_names.txt`.
- **Performance.** Shader and pipeline caches exist (in memory only); the remaining costs are
  per-draw full barriers, one synchronous submit per submission, render-target → texture copies
  and, on multi-GPU systems, the host-memory hop to the presenting GPU.
- **CPU/GPU memory coherence.** Guest-memory textures are re-hashed once per submission (first MiB
  only); render targets written by the CPU or by compute shaders are not tracked yet.
