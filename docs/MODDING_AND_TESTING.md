# Testing shortcuts and the archive overlay

Reaching a scene by replaying the game from the start does not scale: the prologue alone takes about
two minutes, and later episodes would take hours. gr-port therefore has a small set of
**development options** that change what the game loads, without touching the game files, plus a
scenario runner that uses them to capture and compare frames.

This document explains what those options do and, just as importantly, the rules they follow.

## Design rules (read this first)

gr-port must never become a way to distribute a modified copy of Gravity Rush. The options below are
built around four rules:

1. **The original files are never modified.** `/app0` (the game directory) is read-only in the
   virtual filesystem (`loader/kernel/files.cpp`); opening it for writing fails with `EROFS`.
   Overrides exist only in memory, for the lifetime of the process.
2. **Everything is opt-in.** Without `--skip-opening`, `--warp` or `--mods` the game reads its
   archives byte for byte as shipped, and the overlay code is never involved: the default
   command line always runs the unmodified game.
3. **The repository contains no game code or data.** The built-in shortcuts are expressed as
   search-and-replace rules on the user's own Lua scripts at load time
   (`loader/kernel/overlay.cpp`, `ActiveRules()`): only the short text being searched for and
   the replacement live in the source, never a copy of a game file. Loose override files
   (`--mods`) are the user's own and are kept out of git (`/mods/` is ignored).
4. **Shortcuts are for testing, not for playing.** A warp skips story progress: flags, unlocked
   abilities and collected items of the skipped missions are not set. Use it to look at a scene,
   not to play through the game. Nothing produced in these sessions should be shared as "the
   game"; reference frames are kept under `build\` and are not committed either.

If a rule does not find its text (for example in another game version) it is reported in the trace
(`overlay: patch text not found ...`) and the file is served unchanged.

## How the overlay works

The game's data lives in five PSARC archives (`uroot/arc/*.psarc`, 0.8 to 3.5 GB each), read by the
game's own `libSceFios2` through `sceKernelPread`, which gr-port implements. When an override applies
to an archive, opening it returns a **virtual file**:

| Range | Content |
|---|---|
| `[0, header)` | a patched copy of the archive header: the TOC entries of the replaced files point to new data, and their block-table slots describe it |
| `[header, original size)` | the original file, read directly from disk |
| `[original size rounded up to 64 KB, ...)` | the replacement files, stored uncompressed |

The replacement reuses the block-table slots of the original entry (full blocks are marked as
uncompressed with size 0, the last one with its length), so a replacement can be at most as many
64 KB blocks as the original entry. Larger replacements are refused with a trace message. File size
(`fstat`, `stat`) and seeking follow the virtual size. Nothing is written to disk; building the
overlay of an archive reads only its header and the entries being replaced.

## Options

| Option | Effect |
|---|---|
| `--skip-opening` | Removes the falling-apple prologue demos (cuts A, C and the comic cut D) from `event/opening.lua`. New Game reaches the mission system after about 25 s instead of 76 s. |
| `--warp MISSION` | With New Game, starts at `MISSION` instead of `ep01_h`. The patched branch of `Fn_Common_InitMissionTable()` (`param/missionscript_common.lua`) looks the mission up in the game's own `main_mission` table and sets the same state as a retry would (the previous entry becomes the active mission; the episode is the target's). Mission names come from `param/tables/table_missionmain.lua`, for example `ep01_a`, `ep03_a`, `eps1_a`, `free_eps1`, `ep05_a`. An unknown name prints `gr-port warp: unknown mission` and falls back to the normal start. |
| `--mods DIR` | Loose-file overrides: `DIR/<path inside the archive>` replaces that entry in every archive containing it, e.g. `DIR/lua/game/param/init.lua`. For your own experiments and future mod support. |
| `--savedata DIR` | Uses `DIR` instead of `build\bin\savedata` for saves, so tests and experiments never touch your real saves. |
| `--dump-at F1,F2,...` / `--dump-dir DIR` / `--exit-after-dumps` | Writes the listed guest frames as PNG to `DIR` and, with the last option, quits as soon as the last one is written. |

`--skip-opening` and `--warp` both act when New Game is chosen. With the default saves the title menu
appears after the intro movie (about 24 s), and the input `26:UP,27.5:CROSS` selects New Game. The overlay logs every replaced entry,
e.g. `overlay: arc_0_ep_0_0.psarc: lua/game/event/opening.lua replaced from built-in patch`, and the
patched scripts print `gr-port: opening skipped` and `gr-port warp: ep03_a` in the guest output.

## Scenario runner

`tools/run_scenario.ps1` runs the scenarios defined in `tools/scenarios.json`:

```bat
powershell -ExecutionPolicy Bypass -File tools\run_scenario.ps1 all -SaveReference
powershell -ExecutionPolicy Bypass -File tools\run_scenario.ps1 courtyard,ep03
```

Each scenario lists its loader options, a scripted pad input, a time limit and the guest frames to
capture. The input accepts the loader syntax (`seconds:BUTTON[:duration]`) plus a repeat shorthand,
`@from-to/step:BUTTON[:duration]`, handy for skipping dialogue. The runner:

- copies `build\bin\savedata` into the run folder and passes `--savedata`, so every run starts from
  the same saves and never changes the real ones;
- runs without a window (`--no-window`), captures the frames with `--dump-at` and exits as soon
  as the last one exists;
- writes `build\bin\test_out\<scenario>\` with the frames, the trace (`trace.txt`), the report and
  `sheet.png`, a contact sheet of the frames (with the reference frames above them when present);
- with `-SaveReference`, stores the frames as the reference set in `build\bin\test_ref\<scenario>\`;
  otherwise compares each frame with its reference (mean absolute difference of 320x180 thumbnails,
  0-255) and fails above `-Threshold` (default 12).

Current scenarios:

| Scenario | What it reaches | Time |
|---|---|---|
| `title` | the intro movie (Japan Studio, Bluepoint, title animation) and the title menu | ~30 s |
| `opening` | the full prologue (apple taps, comic, movie skipped with CROSS) and the first gameplay | ~2.5 min |
| `movie` | the `ep00-01c` movie after the apple, with subtitles | ~1.5 min |
| `courtyard` | Episode 1 courtyard with Kat, Dusty and the pigeons, prologue skipped | ~1.5 min |
| `ep03` | warp to `ep03_a`: a dialogue, then Kat in the Hekseville alleys | ~1.5 min |

Frame numbers are guest flips, so timing differences between runs move the content slightly; keep
the captured frames on stable moments (gameplay idling, a dialogue box) and compare with a
tolerance. To add a scenario, pick a mission with `--warp`, find good frames with
`--dump-frames 300 --dump-first 1500`, then add an entry to `scenarios.json`.

## Ideas for later

- Save-state style snapshots are out of reach for now (they would need guest memory, threads,
  kernel objects and the GPU state); real game saves are the practical checkpoint. The game
  autosaves at the start of each episode except `ep01_a` (`event/autosave.lua`), so a run that
  reaches Episode 2 produces a save that can be copied into a scenario's `--savedata` folder and
  loaded with Continue.
- The same overlay is the base of Phase 3 (mod support): `--mods` already replaces any archive
  entry, so editable Lua, texture and model replacement only need format tooling on top.
