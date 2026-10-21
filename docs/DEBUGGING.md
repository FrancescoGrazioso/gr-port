# Debugging and tooling reference

Everything here is meant for development. The renderer prints its statistics at exit (draws drawn /
skipped and why, shaders, pipelines, render targets, texture uploads, compute and geometry-pipeline
passes).

## Loader options

| Option | Effect |
|---|---|
| `--run` | runs the game |
| `--no-window` | no window; with `--run` waits `--timeout S` seconds (default 30), then prints where every thread is and the functions called |
| `--input "22:CROSS,23.5:CROSS:0.3"` | scripted presses (seconds since start : button [: duration]); buttons: CROSS CIRCLE SQUARE TRIANGLE OPTIONS UP DOWN LEFT RIGHT L1 R1 L2 R2 L3 R3 TOUCHPAD |
| `--trace N` / `--trace-after S` | maximum trace lines / start writing after S seconds |
| `--trace-lle libSceFios2[,libc]` | also logs calls between the game's own modules (with file paths) |
| `--capture N` | dumps every shader program (VS, PS, CS, ES, GS, fetch) to `build\bin\capture\shaders` and the draw state of submission N to `capture\draws.txt` (`-1` = shaders only) |
| `--dump-frames N` / `--dump-first F` | writes every Nth flipped frame (starting at flip F) to `build\bin\frames\frame_NNNNN.png`; works with `--no-window` |
| `--no-gpu` | PM4 synchronization only, no Vulkan rendering |
| `--frames N`, `--report f`, `--names f` | quit after N frames / linker report / name database |

Reaching gameplay without a pad: `26:UP,27.5:CROSS` (the title menu appears after the ~20 s intro movie), then `CROSS` every 3 seconds from 30 to 420
(apple taps, dialogues; CROSS also skips the movie). Gameplay starts around flip 4500.

**Faster:** `--skip-opening` removes the prologue and `--warp MISSION` starts New Game at any main
mission; `tools\run_scenario.ps1` runs named scenarios built on them and compares the frames with
reference captures. Both are described, with the rules they follow (original files untouched,
opt-in, no game code in the repository), in [MODDING_AND_TESTING.md](MODDING_AND_TESTING.md).

| Testing option | Effect |
|---|---|
| `--skip-opening`, `--warp MISSION`, `--mods DIR` | in-memory archive overlay (see MODDING_AND_TESTING.md) |
| `--savedata DIR` | save directory (default `build\bin\savedata`) |
| `--dump-at F1,F2,...`, `--dump-dir DIR`, `--exit-after-dumps` | capture the listed flips to DIR, quit after the last one |

## Renderer environment variables

GPU work is split into submissions that alternate between rendering and flips, so use odd numbers.

| Variable | Effect |
|---|---|
| `GR_DRAW_TRACE=<submission>` | one line per draw (state, drawn/skip reason), an occlusion-query sample count per draw, every buffer binding (V# word 3, base, stride, bounding box, first values), the textures bound, streamout state and the game's debug markers |
| `GR_DUMP_RTS=<submission>` | every render target and depth buffer at the end of that submission, as PNG, to `build\bin\rt_dump\` |
| `GR_DUMP_TEX=1` | every texture uploaded from guest memory, as PNG, to `build\bin\textures\` |
| `GR_DUMP_GLSL=1` or `<hex address>` | the generated GLSL to `build\bin\glsl\` |
| `GR_RENDERDOC=<submission>` | loads RenderDoc (if installed) and captures that submission to `build\bin\rdoc\gr_capture.rdc`; script it with `qrenderdoc.exe --python script.py` |
| `GR_FIND_VS=<hex>` | logs the submissions that use a vertex shader |
| `GR_DEBUG_NODEPTH=<hex VS>` / `GR_DEBUG_NOKILL=<hex PS>` | disables the depth test / pixel kill for one shader |
| `GR_DEBUG_UNREADABLE_ONE=1` | fills textures that cannot be read with 1.0 instead of 0 |
| `GR_UPLOAD_STATS=1` | lists the shader resources with the most buffer bytes uploaded from guest memory, at exit |
| `GR_PROFILE=gfx[,compute]` or `1`, `GR_PROFILE_FROM=<s>` | sampling profiler (~1 kHz) for the GPU worker threads, starting S seconds after launch; the top functions (self and inclusive) are printed at exit. Frame rate, host time split and draws per flip go to the trace every 5 s (`renderer: N fps at flip ...`) |
| `GR_DEBUG_GS_FLIP=1` | inverts the front face of every geometry-pipeline (ES/GS) draw, to check strip winding |
| `GR_DEBUG_BLACK_TEX=<hex shader>:<resource>` | binds a black texture in place of one texture of one shader (isolate shader terms) |
| `GR_AUDIO_DUMP=<file.wav>` | records the mixed sceAudioOut output (works with `--mute`), so a test can check that sound is produced |
| `GR_NGS2_LOG=1` | NGS2 racks, patches, voice setups, matrices, events and 2-second level statistics (sampler, mixer, master peaks) in the trace |
| `GR_WATCH_RTS=1` | logs the first CPU access to every render target's guest memory |
| `GR_DMA_WATCH=<hex address>` | logs `DMA_DATA` packets that write that address |
| `GR_SETVS_STATS=1`, `GR_DEBUG_SETVS=<hex>`, `GR_SETVS_FROM=<symbol text>` | vertex-shader bind statistics / backtrace of who binds a VS / VS bound while the guest stack contains a symbol |

Shaders that fail to translate are written with their GLSL to `build\bin\shader_errors\`;
unsupported GCN instructions are listed in the trace.

## Guest code instrumentation

| Variable | Effect |
|---|---|
| `GR_BP=<off>[:<reg>:<hex delta>],...` | int3 hit counters on eboot offsets, reported every 5 s; with a register, captures it at the first hit and dumps 8 floats at reg + delta |
| `GR_PEEK=<off>,...` / `GR_PEEK64=<off>:<count>,...` | values of eboot globals |
| `GR_WATCH=<hex addr>:<hex size>[@seconds]` | makes a guest range inaccessible from that time on and logs the first access (rip + address) |
| `GR_CULLDUMP=1` with `GR_BP=26d010:rsi:0` | dumps the game's per-object cull list (AABB, LOD, opacity, visibility) |
| `GR_MATTYPES=1` / `GR_Q1C=1` / `GR_DRAWSKIP=1` | per-material-type, per-queue-subtype and skipped-draw counters in the game's draw path |

## Command-line tools

| Command | Purpose |
|---|---|
| `psarc_tool stats/list/extract <arc.psarc> [out_dir] [filter]` | archive statistics, listing and extraction |
| `self2elf [--info] <in.self> <out.elf>` | fSELF → ELF conversion (open with Ghidra or IDA) |
| `nid_inventory --names data\orbis_names.txt --csv imports.csv <eboot.bin> <*.prx>` | modules, libraries, imports and exports with resolved names; `--nid <name>` computes a NID |
| `gr_disasm --names data\orbis_names.txt <file.elf> <hex address or symbol> [n] [--back B] [--xref]` | annotated disassembly; an imported name resolves to its GOT slot, `--xref` on it gives the PLT stub, `--xref` on the stub gives the callers |
| `gcn_dis [--stats] <shader.bin>...` | GCN (Sea Islands) disassembler for programs dumped by `--capture` |

`tools/ghidra/` holds headless Ghidra scripts used for the reverse-engineering work:
`Decomp.java` (decompile the functions containing eboot offsets), `StrRefs.java` (string →
references → decompiled callers), `ScalarRefs.java` (functions using given constants) and
`AddrRefs.java` (references to addresses). Example:

```bat
analyzeHeadless <project dir> gr -import eboot.elf
analyzeHeadless <project dir> gr -process eboot.elf -noanalysis -scriptPath tools\ghidra -postScript Decomp.java out.c 26d010
```

## Adding an HLE function

In `loader/hle_functions.cpp` or a file under `loader/kernel/`:

```cpp
h.Register("sceKernelIsNeoMode", [](const uint64_t* a) -> uint64_t { return 0; });
```

`a[0..5]` are the SysV registers `rdi, rsi, rdx, rcx, r8, r9`, `a[6]`, `a[7]`, `a[9]` the first
three stack arguments, `a[8]` the caller's return address and `a[10..13]` the low 64 bits of
xmm0-3 (`ArgF(a, i)` / `ArgD(a, i)` read float/double arguments). Guest pointers are host pointers.
Use `CallGuest(fn, {args...})` to call back into guest code (callbacks, thread entries) and
`RegisterNid(nid, fn)` for symbols without a known name.
