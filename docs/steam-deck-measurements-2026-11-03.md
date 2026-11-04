# Steam Deck measurements, 2026-11-02/03 (full log)

The working log behind docs/steam-deck-performance.md, which has the conclusions: every race window, profile table and source line the summary was cut from. File and line references are at the commits named in each section; the per-draw changes it measures are upstream, and the constant-ring cache it measures was not applied.

## Steam Deck performance — where the time goes (2026-11-02)

Goal: a solid 60 FPS in HD races on the Steam Deck (LCD, 1280x800, 4 cores / 8 threads, 15 W shared
between CPU and GPU). Measured on build 0.2.50 plus the compile-policy branch, Vineta K, full races,
`GOW3_FRAME_STATS=1`, settings as the launcher left them: render resolution 720p, upscaler FSR 3.1
preset Quality, output resolution 1920x1080, vsync, frame limit 60, async shaders on. Scripts:
`tools/deck/ab-run.sh` (frame stats, per-thread CPU and GPU busy every 6 s, live toggles),
`tools/deck/perf-run.sh` (the same plus `GOW3_GPU_PROFILE`), run from Konsole in desktop mode;
`tools/deck/exp-run.sh` (launch variables as arguments, started over ssh as a `systemd-run --user`
service) and `tools/deck/pin-threads.sh` (pins the four hot threads, the three GPU threads plus the
game's main render thread, the first of its four `Graphics::Rende` threads, to separate cores).

## Measurements

Race, before any change: 36–53 FPS. 1500–4200 draws per frame (the pack start is the top end),
the GPU command thread 6–10 µs per draw and never idle, GPU busy 99–100 % (`gpu_busy_percent`),
GPU clock 880–1600 MHz, 4.5–5 cores' worth of threads runnable.

`perf record -e cpu-clock:u` over 20 s of a race (share of all samples; the process used about
4 threads' worth of CPU):

| thread | share | what |
|---|---|---|
| gow3:DrawRec (draw pipe stage B) | 17 % | binds, barriers, render state: spread over 15 functions, none above 7 % of the thread |
| gow3:VkRecorder | 17 % | 8.6 % of all samples inside the RADV driver, 5.9 % in libc memcpy (uploads) |
| shadPS4:GpuComm (command thread) | 13 % | PostDraw 13 %, TryPreparedPipeline 10 %, ProcessGraphics 10 %, register packets 8 %, ring bindings 5 %, sched_yield 3 % (of the thread) |
| Graphics::Render (the game's) | 11 % | 12.5 % of it in DrawPreparation::Build, 5.8 % guest_memcpy |
| gow3:DrawPrep0–3 (speculative, SCHED_IDLE) | 4 × 4.9 % | GetProgram, StageSpecialization, ParseFetchShader |
| gow3-probe main thread | 6 % | game code |
| shadPS4:GpuSche | 6 % | 98 % of it `tdefl_compress`: the PNG of `GOW3_SCREENSHOT_EVERY` (see Hygiene) |
| AK::EventManager (the game's audio thread) | 5.6 % | the port's ATRAC9 decoder (RunImdct, UnpackFrame) |

`top -H` during the race: GpuComm 91–94 % of a core, DrawRec 67–87 %, VkRecorder 67–73 %,
AK::EventManager 30–42 %, main 16–31 %, Graphics::Render 17–28 %, each DrawPrep worker 20–24 %.

### Live A/B, one race per table, switched between laps

Runtime toggles (`GOW3_TOGGLE_FILE`), at equal draw counts:

| lap | setting | FPS |
|---|---|---|
| 1 | default | 50 at 2100 draws, 44 at 3300 |
| 2 | draw preparation off (mask 12288) | 49–52 at 2200 draws (same), 55 at 1470 |
| 3 | threaded Vulkan recording off (mask 16) | 52 at 1550, 47 at 2200, 41 at 3000 (same or 1–2 lower) |

Neither CPU switch changes the frame rate: the GPU is the first limit.

Upscaler preset and output size (`GOW3_PRESET_FILE`):

| lap | setting | FPS | command thread waiting for the GPU |
|---|---|---|---|
| 1 | Quality (nominal 1280x720), output 1920x1080 | 47–53, 40 at 3600 draws | 5–25 % |
| 2 | Performance (nominal 960x540) | 50–56 | 2–20 % |
| 3 | Native AA (1920x1080) | 44–49, 42 at 3300 | 12–33 % |
| 4 | Quality, **output 1280x720** | **58–60 (vsync), 53–56 at 3200 draws** | 0–3 % |

### The 720p raster does not happen

`GOW3_SCENE_DEBUG` on a race frame printed no reduced pass: `scene_started` is only set by a pass
with five or more colour targets and a 1920x1080 depth (God of War III's G-buffer,
`vk_rasterizer.cpp` BeginRenderingFull). WipEout's scene passes have three (B10G11R11 colour and
two R16G16 motion vectors, D32 depth), so no pass ever draws into the reduced proxies. The presets
only shrink the upscaler's input: the native 1920x1080 targets are copied down into proxies
("scene proxy fill") and FSR scales them back up. That is why Performance and Native AA differ by
only a few FPS, and it costs GPU time instead of saving it.

The game-side "Resolution Patch - 720p" is applied (log: 230 writes) but the game still registers
1920x1080 display buffers (`RegisterBuffers ... width = 1920, height = 1080`) and the race frame's
passes are 1920x1080. The maintainer verified the patch with 1280x720 screenshots on Windows; on the
Deck the port reports 1920x1080 in `sceVideoOutGetResolutionStatus` (output resolution setting),
which is the likely reason the game stays at 1080p. Not yet verified: a launch with
`output_res=1280x720` in gow3.ini.

So on the Deck today the race is rasterized at 1920x1080, downscaled, upscaled to 1920x1080 by FSR,
composed with the HUD at 1920x1080, then scaled to the 1280x800 panel.

## Areas, in order of expected payoff

1. **Output resolution on the Deck.** Lap 4 above: 1280x720 output gives 58–60 FPS at once. The
   output options are 720p/1080p/1440p/4K (`gow3_settings.h` OutputWidths); add 1280x800 (the
   panel) and default the output to the display size on an integrated GPU, or expose the row the
   launcher has commented out. A settings change only, no risk.
2. **Rasterize the race at the chosen size.** Two ways, pick one:
   - make the game's 720p (or 800p: the patch generator handles 16:10) resolution patch take
     effect by reporting the matching resolution to the game at start; verify with the
     `RegisterBuffers` width in the log and a 1280x720 screenshot;
   - or give WipEout its own scene signature so `scene_started` fires on its 3-target pass with the
     full-size depth (per game profile; the proxies and FSR path then do real work).
   Expected: the main passes at 2.25x fewer pixels; combined with 1 the GPU should have headroom
   for the pack start. Measure with the per-lap protocol.
3. **CPU per draw for the pack start.** With the GPU out of the way, lap 4 still fell to 53 FPS at
   3200 draws: 5.3 µs per draw on the command thread is 17 ms a frame. Targets, from the profile:
   PostDraw and TryPreparedPipeline on the command thread, the register packet apply/hash, the
   driver and memcpy time on the recording thread, binds and barriers on stage B. A 25 % cut holds
   60 FPS in the pack.
4. **Fewer threads on 4 cores.** The 4 draw-preparation workers and the scanner take about a
   core and their results did not change the frame rate on the Deck (lap 2 of the toggle table);
   they also eat power the GPU clock could use (the clock dropped to 1000 MHz when the CPU was
   loaded). Run without them below 12 hardware threads, or make the count a setting (applied 2026-11-03
   as ≤ 8 hardware threads, see CPU experiments). The ATRAC9
   decoder at 30–42 % of a core on the audio thread is the next candidate (SIMD IMDCT, or batch).
5. **Frames queued.** With one frame queued the command thread waited for the GPU 5–33 % of the
   time in laps 1–3; two frames would overlap more at the cost of latency. Cheap ini A/B.

## Hygiene (do first, they distort every measurement)

- `GOW3_SCREENSHOT_EVERY` encodes PNGs with miniz on the GPU scheduler thread
  (`vk_presenter.cpp`): about 70 ms per screenshot, so every per-second screenshot run of the lap-1
  investigation carried a 70 ms hitch each second and the "worst frame" numbers of those runs are
  wrong. Move the encode to a worker thread (also removes the F12 hitch for players).
- `GOW3_GPU_PROFILE` prints 0.1–0.4 ms per frame during races: it skips frames whenever the GPU is
  behind (`paused` in BeginFrame) and counts frames per NoteFrameStart, so under a saturated GPU
  it measures almost nothing. Needed for any per-pass GPU work; fix the accounting first.
- The frame-stats "reduced-size draws" counter only counts passes that take the full
  BeginRendering path, so "0 of 0" cannot be read as proof; `GOW3_SCENE_DEBUG=<file>` is the test.
- `perf` works on the Deck as the deck user (`perf_event_paranoid=2`, user-space samples only):
  `perf record -e cpu-clock:u -F 999 -g -p $(pgrep -f gow3-probe) -- sleep 20`, report with
  `--sort comm` first, then `--comm <thread> --percentage relative`.

## Changes on this branch (2026-11-02, not yet run on the Deck)

- `vk_scene_signature.h`: WipEout's scene pass (colour targets 0–2 only, B10G11R11 colour,
  R16G16F motion vectors, both the depth's size) starts the reduced-size scene in
  BeginRenderingFull under the game profile; toggle bit 56 (`WipeoutSceneRaster`, mask
  72057594037927936) turns it off for A/B runs. Host test `scene-signature-test`.
- Launcher: the Output resolution row is back; on a Deck (DMI product Jupiter/Galileo) a gow3.ini
  without `output_res` gets 1280x720, also on the Steam `--play` path.

### Verified on the Deck, 2026-11-03 (1080p output, FSR 3.1 Quality)

- The rule fires: the scene debug frame prints `Scene candidate: mrt 0x7 ... signature 1
  scene_started 1` and 36 `Scene pass: reduced 1` passes (the depth-only prepass and the post passes
  stay native). The 2026-11-02 "no reduced pass" print was an arming bug: the flag was re-decided at
  every HUD draw into the display buffer; it is now decided once per display buffer.
- Picture intact: HUD crisp, track, bloom and pause screen normal in 1280x720 screenshots with the
  rule on and off.
- Frame rate: at equal draw counts the 720p raster gives +1-2 FPS (52 vs 50 at 2150 draws, 57 vs 56
  at 1250); GPU busy stays 99 %. WipEout's scene geometry is not where the Deck's GPU time goes.
- **FSR 3.1 is the GPU cost.** Same race, preset file `1 1 0` (upscaler off, which also returns the
  raster to native 1920x1080): **60.0 FPS** at 1250-2220 draws, 55.6 at 3140, command thread never
  waiting for the GPU, GPU clock down to ~1000 MHz with slack; `1 1 1` (FSR 3.1 back on): 52-56 FPS at
  the same draws, clock 1200-1600 MHz. FSR 3.1 at a 1080p output costs the Deck about 4-6 ms a frame,
  more than the whole scene raster. That is also why the 1280x720 output reached 60 FPS on 2026-11-02.
- Still CPU-bound at the pack start: 3100-4300 draws at 5.3-8.7 µs give 26-44 FPS with any GPU setting.

### Anti-aliasing options, same race, 2026-11-03 (preset file `preset output provider`)

| lap | setting | FPS at 1400–2400 draws | pack start |
|---|---|---|---|
| 1 | `0 1 4` port TAA, native 1080p raster, 1080p output | **57–60** | 47 at 3200–3500, 37 at 4250 |
| 2 | `0 0 4` port TAA, 720p raster, 720p output | **60 flat**, 57–58 at 1900–2250 | 54 at 3300 |
| 3 | `1 1 1` FSR 3.1 Quality, 720p raster, 1080p output | 50–55 | 45 at 3200 |

The port's TAA costs about a millisecond where FSR 3.1 costs 4–6, and its picture is the native
raster (screenshot clean, no ghosting on the ship or rails). Under TAA the preset is forced to
Native AA (`RenderPreset`), so the reduced raster is not used with it; the reduced raster matters
for FSR 3.1 only, where it buys 1–2 FPS.

**Recommendation.** On the Deck's own screen: output 1280x720 and TAA, a flat 60 outside the
pack start. On a 1080p TV: TAA at the 1080p output, 57–60 FPS outside the pack start, sharper than
FSR Quality. FSR 3.1 is the wrong default for this GPU. What remains is the CPU per draw at the
pack start (areas 3 and 4 above): 3200–4250 draws at 5.3–8.7 µs is 20–25 ms a frame on the command
thread whatever the GPU does.

## CPU experiments (2026-11-03)

| experiment | pack start (≥3000 draws) | 1500–2500 draws | GPU clock | verdict |
|---|---|---|---|---|
| workers off (`GOW3_PREP_WORKERS=0`) vs default (4 workers + scanner) | 51 vs 38 at 3950–4000, 50 vs 40 at 3800–3940; later laps 56 vs 48 at 3590–3720, 60 vs 53 at 3070–3250 | 59–60 vs 57–60 (vsync cap); 59–60 vs 55–56 at 2650–2950 | 200–1040 (median 600) vs 360–1190 MHz (median 1040) | none on ≤ 8 threads |
| hot threads pinned to separate cores in lap 2 (pin 11:35:26 / release 11:36:23, Deck `date`): GpuComm CPUs 0,1, DrawRec 2,3, VkRecorder 4,5, game render thread 6,7; vs unpinned lap 1, lap 3 and a discarded first attempt, default workers | no pinned window ≥ 3000; unpinned only: 38 at 3570 (lap 1), 44 at 3860 (first attempt) | 60 vs 60 at 1440–1760 (vsync cap); 59.6 vs 58.8–59.8 at 2340; 56.0 vs 57.1–59.8 at 2530, 58.3 vs 57.1–57.8 at 2670 | 200–1080 MHz (median 1040) vs 200–1040 (median 962–999) | not applied |
| two frames queued (`GOW3_FRAMES_AHEAD=2`) vs 1 frame queued (default), default workers | 44 vs 40 at 3620–3940, 41 vs 38 at 3950–4170 (GPU wait 0.0–0.4 vs 6.4–6.6 %); later lap 53 vs 53 at 3250–3350 | 59.4 vs 59.7 mean of 12 pairs (vsync cap), GPU wait 0.0–0.2 vs 0.0–1.7 %; 55–58 vs 55–56 at 2850–2950 | 200–1070 MHz (median 840) vs 360–1190 (median 1040) | not applied (within run-to-run noise); lag: "The controls felt fine" |

Verdict: Task 5 applies "no workers on ≤ 8 hardware threads": the pack start gains 10–13 FPS (4.7–5.1 µs per draw instead of 5.7–6.1) with the GPU at a lower clock, and nothing is lost at 1500–2500 draws, where both runs are at or just under the 60 FPS vsync cap.

One Vineta K race each, TAA, 1280x720 output, on the installed deckperf build (GitHub run 41620135283, built at commit f174520; 4 draw-preparation workers by default), the same build as the "Command thread at the pack start" profile. Windows are the 5 s `Frame stats` windows of the race, paired one to one within ±10 % draws, without each run's load-into-race window; two lines with the same stamp are two consecutive windows (stdout arrives in blocks).
`top` cuts thread names to 8 characters: `gow3:Dr+` is 6 threads in the default run (DrawRec, DrawPrep0–3, DrawScan) and only DrawRec with workers off, where the scanner is not started either.

Verdict (pinned): Task 6 does not pin threads in code. Pinned lap 2 gains nothing at matched draws (−1 to −4 FPS at 2530, +0.5 to +1.2 at 2670, 60 vs 60 at the vsync cap), and the evidence is thin: one pinned lap, two windows below the cap, both next to a pause. No pinned window reached ≥ 3000 draws, so the pack start was not tested pinned: this is a null result mid-race, not at the pack start. The run also pinned the game's render thread, which the GPU library cannot pin, so Task 6 could pin only three of the four threads anyway.

Pinned run: both switches were made in the pause menu (identical draws, idle 47–53 %), so no race window straddles one; pauses, loads and the pause-onset window (idle 35 %) are left out. Lap 1's 52.3 at 2705 is the window right after the pack start and is not counted against lap 2's 58.3 at 2666.

Verdict (frames queued): Task 7 does not make 2 frames queued the Deck default. With 2 queued the pack start gains 3.6 FPS in both pairs and the command thread's GPU wait (`GPU ticks` in the frame stats, where the frames-queued bound waits) falls from 6.4–6.6 % to 0.0–0.4 %, but one frame queued alone gave 38–40 at 3940–3950 (Task 2 control) and 43.6 at 3860 (Task 3's first attempt, default settings), a 3.5–5.8 FPS spread between two races at matched draws that brackets the +3.6, so the gain is inside run-to-run noise. The control's command thread is about 0.2 % idle at the pack start, so removing its 6.4–6.6 % GPU wait can give at most about 7 % more frames (37.8 → 40.4, 40.1 → 42.9); the observed 41.4 and 43.7 exceed that bound, so part of the +3.6 is noise. Mid-race the 60 FPS vsync cap hides any gain (59.4 vs 59.7), 2850–2950 draws gain +0.3 to +2.4, and the later-lap ≥ 3000 pair is −0.4. The variant's GPU clock was lower, as with workers off, which works against it.

Frames queued: one race each, Task 2's control and this one. No log line names the setting: `env.txt` has `GOW3_FRAMES_AHEAD=2`, and the GPU wait is 0.0 % in 23 of the variant's 25 race windows.

## Command thread at the pack start (2026-11-03)

`tools/deck/perf-start.sh` on `shadPS4:GpuComm`, one Vineta K race, 1280x720 output, the installed build's defaults: 4 draw-preparation workers (`draw preparation workers: 4` in the log; Task 5's 0 is not built yet). Slice 12:14:45.1–12:15:05.1 (Deck clock), the whole pack start, 2700–4015 draws at 39.5–44 FPS: 22.7K `cpu-clock:u` samples at 1999 Hz, so the thread ran user code 57 % of the wall time. Shares are of those samples (`--percentage relative`). The library has symbols but no line tables: lines come from the disassembly read against the source. perf splits code it cannot name into one row per address; here those rows are merged into the function they belong to. Frame-pointer unwinding skips the direct caller of a libc or xxhash leaf, so their callers come from the return addresses.

| symbol, self time | % | source |
|---|---|---|
| Rasterizer::PostDraw | 13.6 | vk_rasterizer.cpp:627 |
| memcpy/memmove (libc, no symbols; perf rows 0x170bc5, 0x170be2, ...) | 11.1 | callers below |
| ProcessGraphics coroutine body (no symbol, 0x15eeac–0x162bf0; perf rows 0x15eff8 2.2, 0x15efe3 1.3) | 10.2 | liverpool.cpp:729 |
| PipelineCache::TryPreparedPipeline | 10.0 | vk_pipeline_cache.cpp:671 |
| memcmp (libc AVX2; perf row 0x1704f2 2.5) | 5.2 | vk_pipeline_cache.cpp:695, :700 |
| Liverpool::ApplyGraphicsRegisterPacket | 5.2 | liverpool.cpp:504 |
| malloc/free (libc internals and cfree) | 3.9 | 2.5 of it under RetireSubmission's keepalive pops, vk_rasterizer.cpp:746 |
| Liverpool::HashRegisterPacket | 3.4 | liverpool.cpp:496 |
| Rasterizer::CollectRingBindings | 3.3 | vk_rasterizer.cpp:567 |
| __sched_yield (libc) | 3.2 | DrawPipe::Drain, vk_draw_pipe.h:109 |

Next: XXH3 (libxxhash 0x524e, the GraphicsPipelineKey hash, vk_graphics_pipeline.h:147) 2.2, DrainDrawPipe 2.0, PendingWriteOverlaps 1.9, ProcessCommands 1.5. By library: libgow3gpu 67.5 %, libc 24.6 %, libxxhash 3.6 %, the RADV driver 0.2 %.

Callers of the top five:
- PostDraw ← Rasterizer::Draw ← ProcessGraphics (draw packets): 13.4 of 13.6.
- memcpy: CopySparseMemory for the constant ring (from CollectRingBindings, vk_rasterizer.cpp:591) 6.0, PostDraw's packet copies 2.1, ApplyGraphicsRegisterPacket's register payloads (liverpool.cpp:527–552) 2.0, TryPreparedPipeline 0.8.
- ProcessGraphics body ← Liverpool::Process (coroutine resume): all of it.
- TryPreparedPipeline ← GetGraphicsPipeline ← Draw: 9.9; its children add XXH3 2.2, memcmp 5.0 and memcpy 0.8, about 19 % in all.
- memcmp ← TryPreparedPipeline 5.0 (the flat user data at vk_pipeline_cache.cpp:695 and the 352-byte key compare in `graphics_pipelines.find`, :700).
- The next three by perf's own rows: ApplyGraphicsRegisterPacket ← the PM4 loop (liverpool.cpp:778), all of it. HashRegisterPacket ← ApplyGraphicsRegisterPacket (:622), all of it; it is a frameless leaf, so the unwinder shows the PM4 loop. CollectRingBindings ← PostDraw (vk_rasterizer.cpp:654), 3.2 of 3.3.

Hot spots:
- PostDraw: copy of the dirty 32-word register blocks into the packet, vk_rasterizer.cpp:696–700, 4.2. `dirty.Clear()` (`rep stos` over the 208-byte mask, regs.h:238) at :736, 1.5. The mask scan `dirty.ForEach` at :639 (regs.h:225), 1.5. First touch of the pipeline in `GetStages()` at :641, 1.4. The `thread_local` rings at :643 (`__tls_get_addr` 0.7, most of it called from PostDraw). DrawPipe Begin/Commit (vk_draw_pipe.h:63, :84), 1.2.
- TryPreparedPipeline: loads of the PreparedDraw a worker wrote (state, checksum, stage count, vk_pipeline_cache.cpp:672–676), 4.0. These are most likely cache misses on lines another core wrote. `GetParams` reads each shader's footer in guest memory (:686, regs_shader.h:224), 2.9. Map probe 1.4.
- ProcessGraphics: the next packet's header load and NextPacket (liverpool.cpp:757–761), 3.5. `ProcessCommands()` is an out-of-line call per packet (:758, :90), 1.5.
- HashRegisterPacket: its dependent multiply chain (liverpool.cpp:497–500). CollectRingBindings: first touch of the stage's Shader::Info (:570–575), 1.2.
- DrainDrawPipe and __sched_yield (5.1 together) all come from OnSubmit's drain (vk_rasterizer.cpp:1569), where the thread waits for stage B.

Mid-race (12:15:10–12:15:30, 1770–2480 draws, 58–60 FPS) has the same order within 0.5 points (PostDraw 13.1, TryPreparedPipeline 10.0, apply 5.5, ring bindings 3.7, hash 3.3). These are per-draw costs; no fixed per-frame cost stands out.

Who sets the pace. Every pack-start window of this race shows stage A (this thread) waiting 33–39 % of the time for stage B at OnSubmit:1569, with stage B (`gow3:DrawRec`) busy 92–95 %. The workers-off race in the CPU experiments above shows 20–22 % and 94–95 % at 3590–4000 draws. The profile sees only about 5 % of that wait, the spin plus the user side of `sched_yield`; the rest is kernel and off-CPU time. So stage B, not this thread, sets the pace at the pack start. A cut below shortens frames only as far as stage B keeps up. "Busy" is wall time inside B's packet handlers and includes time B is blocked or preempted (`top -H` above has DrawRec at 67–87 % of a core). Whether B is short of CPU or waits on something else is for a DrawRec profile, with the same script and `PERF_TID`. A `cpu-clock:u` profile will not see B's blocked time.

Brief. Self shares are upper bounds on what a change can save on this thread. (1) **PostDraw's packet**, ≤ 8 %: block copy 4.2, Clear 1.5, mask scan 1.5, TLS 0.7. The changes: clear and scan only the mask words that were set, make `rings` a Rasterizer member, and send the dirty words rather than whole 32-word blocks. This is first because it is the one change that also takes work off stage B, which copies the same blocks again in RunDrawPacket (vk_rasterizer.cpp:783). (2) **No register checksum without draw preparation**, ≤ 3.4 % (HashRegisterPacket). Only the prepared path reads `gfx_reg_checksum` (vk_pipeline_cache.cpp:673, vk_rasterizer.cpp:1004, vk_draw_prep.cpp:246), and `Enabled()` is false at 0 workers, now the Deck default. The workers share ApplyGraphicsRegisterPacket, so the skip goes on the GPU thread's call (liverpool.cpp:778). (3) **Constant ring copies**, up to 6.0 %: CollectRingBindings is 12.6 % inclusive, and 6.0 of that is memcpy of guest constant buffers through CopySparseMemory (vk_rasterizer.cpp:591). Copy a range once per submission when several draws bind it. Count the repeats first: the gain is that share times the repeat rate. Not first: TryPreparedPipeline with its memcmp and XXH3 (about 19 %) and RetireSubmission's frees (3.7 % inclusive) belong to the 4-worker path, which no longer runs on the Deck by default. At 0 workers the regular path (RefreshGraphicsKey → GetProgram, 3.0 % here for the ~8 % of draws that missed a prepared pipeline) takes its place, and this profile does not measure it at full rate. A profile of the Task 5 build will reorder this table.


## Stage B at the pack start (2026-11-03)

`tools/deck/perf-start.sh` on both pipeline threads at once, one Vineta K race, 1280x720 output, the installed build's defaults: 0 draw-preparation workers (`draw preparation workers: 0 (8 hardware threads)` in the log). The build is GitHub run 41642874892 at commit 108143a, which has none of Tasks 2–6 of the per-draw plan; every line below is at 108143a. Recording 15:06:57.4–15:07:37.4 (Deck clock), 106.5K `cpu-clock:u` samples at 1999 Hz with `-g`.

The slice. Fewer than 20 s of the recording are at ≥ 3000 draws, so the slice is only those seconds. A `Frame stats` stamp is the time the line reached game.log after stdout's block buffering, 2.5–4 s after its window ended, so the windows were placed by the profile itself: both threads' sample rate drops about 10 % and stage B's spin (below) rises from 2–6 % to 10–16 % of its samples a second in the 2179-draw window, and the sample rate falls from 15:07:20 in the 1826-draw window. The pack start is 3744 draws at 49.1 FPS (stamped 15:07:06.531, about 15:06:59–15:07:04), 3298 at 50.2 (15:07:18.074, about 15:07:09–15:07:14) and 3107 at 58.3 (15:07:23.082, about 15:07:14–15:07:19). Left out: 2851 at 53.0 (15:07:00.281, mostly before the recording), 2179 at 58.6 (15:07:12.677) and 1826 at 60.0 (15:07:26.524). Window edges are good to about 1 s. Against a slice trimmed by those edges, the full 15 s swaps two pairs of neighbours at places 7–9 and moves B's spin by 1.4 points, so the slice used is the trimmed one: 15:06:59–15:07:03 and 15:07:12–15:07:19, 11 s, 17.7K samples of stage B and 17.5K of stage A (80 % of the wall time in user code for each).

The `Draw pipe` lines in the same stdout blocks: stage B busy 83.7 % (3744 window), 90.1 % (3298) and 92.2 % (3107); stage A waited 14.0, 14.1 and 25.9 %, all at OnSubmit:1569. In the left-out 2179 window B was busy 75.6 % and A waited 8.4 %.

Method. `perf script` of the recording, cut to the slice; shares are of each thread's samples in it, B's own spin included. perf on the Deck names only the library's exported symbols; local ones (`t` in `nm`) come out as one row per address. Here those rows are mapped with `nm -S` of the same library (build ID 35c5b98d) and merged into their functions; libc rows are placed between the start addresses of its `memcpy`, `memcmp` and `memset` ifunc variants. The plan's `--symbol-filter=0x000000000016a50a` matches nothing for the same reason, so callers come from the dump too. Frame-pointer unwinding skips the direct caller of a frameless leaf (`__tls_get_addr`, `memmove`, `memcmp`, `malloc`), so their callers are the functions called at the return site found.

Stage B (`gow3:DrawRec`):

| symbol, self time | % | source |
|---|---|---|
| Rasterizer::BindTextures | 7.3 | vk_rasterizer.cpp:2382 |
| Rasterizer::RunDrawPacket | 6.8 | vk_rasterizer.cpp:767 |
| DrawPipe::Run waiting for the next packet (no symbol; perf row 0x16a50a 6.3 of the unsliced 20 s) | 4.4 | vk_draw_pipe.h:186–201 3.8, the rest of Run 0.6 |
| Rasterizer::BeginRendering | 4.2 | vk_rasterizer.cpp:2803 |
| Image::GetBarriers | 3.7 | image.cpp:261 |
| Rasterizer::PrepareRenderState | 3.4 | vk_rasterizer.cpp:247 |
| Rasterizer::DrawRecord | 3.0 | vk_rasterizer.cpp:1046 |
| Rasterizer::CachedImageDescEntry | 2.9 | vk_rasterizer.cpp:2340 |
| Pipeline::BindResources | 2.8 | vk_pipeline_common.cpp:33 |
| Rasterizer::BindResources (`.part.0` clone, no symbol) | 2.7 | vk_rasterizer.cpp:1692 |

Next: VertexAttribute::GetSharp 2.3, GetVertexInputs 2.2, `__tls_get_addr` 2.2, TouchImage 2.1, BindBuffers 2.0, ResolveVertexBuffers 2.0, memmove 1.8, UpdateViewportScissorState 1.8, BeginRenderingFull 1.6, MakeBeginSignature 1.6, Scheduler::ReserveRecordData 1.5, memcmp 1.5, ImageResource::GetSharp 1.4, malloc/free 1.3, mutex lock/unlock 1.3. By library: libgow3gpu 88.9 %, libc 7.1 %, ld.so 2.2 %, the RADV driver 0.4 % (B writes commands into the record buffer; `gow3:VkRecorder` makes the Vulkan calls). Inclusive: DrawRecord 82.2, BindResources 27.7, BindTextures 20.7, ResolveVertexBuffers 11.6, BeginRendering 9.6, ObtainBuffer 5.8, PrepareRenderState 5.7, GetVertexInputs 5.2, UpdateDynamicState 5.2, Transit 4.3.

The spin is not B's work: it is B waiting for stage A's next packet (`busy` in the Draw pipe line counts only the handler, vk_draw_pipe.h:209–211).

Stage A (`shadPS4:GpuComm`), same slice:

| symbol, self time | % | source |
|---|---|---|
| Rasterizer::PostDraw | 10.4 | vk_rasterizer.cpp:627 |
| memcpy/memmove (libc) | 8.8 | callers below |
| ProcessGraphics coroutine body (no symbol) | 7.6 | liverpool.cpp:729 |
| StageSpecialization::operator== (no symbol) | 7.1 | specialization.h:195 |
| PipelineCache::GetProgram | 6.6 | vk_pipeline_cache.cpp:1173 |
| StageSpecialization constructor (no symbol) | 5.3 | specialization.h:95 |
| RefreshGraphicsStages, the per-stage lambda (no symbol) | 4.3 | vk_pipeline_cache.cpp:995 |
| Liverpool::ApplyGraphicsRegisterPacket | 4.2 | liverpool.cpp:504 |
| Gcn::ParseFetchShader | 3.8 | fetch_shader.cpp:95 |
| BufferResource::GetSharp (no symbol) | 3.4 | resource.h:91 |

Next: HashRegisterPacket 2.9, RefreshGraphicsKey 2.6, ImageResource::GetSharp 2.4, BuildRuntimeInfo 2.0, PublishGraphics 2.0, memcmp 2.0, `__tls_get_addr` 1.6, XXH3 1.5, GetGraphicsPipeline 1.4, NextPacket 1.2, CollectRingBindings 1.2, `__sched_yield` 1.1. By library: libgow3gpu 79.6 %, libc 14.4 %, ld.so 1.6 %, libxxhash 1.5 %.

Against the 4-worker table above: TryPreparedPipeline (10.0) and its memcmp and XXH3 are gone, and the regular pipeline selection runs for every draw: RefreshGraphicsKey is 44.4 % inclusive (GetProgram 33.5, StageSpecialization's constructor 17.0 with ParseFetchShader 4.2, its `operator==` 7.1). PostDraw 13.6 → 10.4, memcpy 11.1 → 8.8 (the constant ring 6.0 → 4.8 here, 6.5 over 15:07:00–15:07:20; at 0 workers A has more samples, so a lower share is not a lower cost), ApplyGraphicsRegisterPacket 5.2 → 4.2, HashRegisterPacket 3.4 → 2.9 (out of the top 10), `__sched_yield` 3.2 → 1.1: A waits less for B (14–26 % against 33–39 %).

A's leaves: memmove ← CopySparseSerial ← CopySparseMemory, the constant ring under CollectRingBindings (vk_rasterizer.cpp:591), 4.8; ApplyGraphicsRegisterPacket's payloads 1.7; PostDraw 1.6; RefreshGraphicsStages 0.6. PostDraw's self time: block copy (:696–700) 3.4, mask scan (:639) 1.3, `dirty.Clear()` (:736) 1.0, the sequentially consistent store in DrawPipe::Commit (vk_draw_pipe.h:87, an `xchg`) 0.9, first touch of the stages (:641) 0.8; its `thread_local` rings (:643) add 0.4 of `__tls_get_addr`.

Callers of stage B's top five, and of the sixth, since the spin has none:
- BindTextures ← Rasterizer::BindResources (vk_rasterizer.cpp:1752, the serial loop over stages) ← DrawRecord (:1108): 7.24 of 7.32.
- RunDrawPacket ← DrawPipe::Run's handler call (vk_draw_pipe.h:210): all of it.
- The spin is the thread's own loop: no caller.
- BeginRendering ← DrawRecord (:1115): 4.13 of 4.18.
- GetBarriers ← Runtime::Transit (vk_runtime.cpp:201) ← BindTextures, the sampled-texture transition with the view's range (vk_rasterizer.cpp:2612), 3.15; the other Transit calls of BindTextures 0.14, BeginRenderingFull 0.2, ClearImage 0.08.
- PrepareRenderState ← DrawRecord (:1089): 3.30 of 3.35.
- B's leaves, by the function at the return site: `__tls_get_addr` (2.2) ← VertexAttribute::GetSharp 0.53, BindResources 0.29, RunDrawPacket 0.25, ImageResource::GetSharp 0.16, SamplerResource::GetSharp 0.12, DrawRecord 0.12, then PrepareRenderState, BeginRendering, ResolveIndexBuffer and UpdatePrimitiveState under 0.1 each. These are `Shader::Info`'s user-data snapshots read through `Snapshot()` (info.h:174–184) by every GetSharp, the `stage_regs` pointer read by `Regs()` (vk_rasterizer.cpp:86–87, vk_rasterizer.h:495), and RunDrawPacket's own writes of both (:795–799, :869, :893). None of it is PostDraw's rings. memmove (1.8) ← EmitVertexBuffers 0.88, Pipeline::BindResources 0.62. memcmp (1.5) ← PrepareRenderState 0.65, MarkPass 0.35, Scheduler::BeginRendering 0.27, BeginRendering 0.24. malloc/free (1.3) ← DrawRecord 0.92: `GetFetchShader()` returns `std::optional<const FetchShaderData>`, `GetDrawOffsets` (:377) takes `std::optional<FetchShaderData>`, so :1232–1233 copies the attribute vector into a temporary every draw. Mutex lock/unlock (1.3) ← TextureCache::ResolveReadbacks under ObtainBuffer 0.48, TextureCache::GetSampler 0.39.

Hot spots:
- RunDrawPacket: the copy of the 32-word register blocks into B's register file, vk_rasterizer.cpp:782–786, 4.6. These are the first loads of packet bytes stage A wrote on another core. The stage loop (:788–800) 1.2, the end of the packet (:892–897) 0.7.
- BindTextures: the start of the per-binding path, four container clears, `++bind_epoch` and the 16-entry `resolved{}` (:2401–2409), 1.5; the binding memo check (:2478–2479), the first load of the cache entry, 1.2; the usage bits and layout (:2617–2624) 0.6; the rest is spread. The texture-set memo (BindTexturesFromSet, :2717) needs a PreparedDraw, so at 0 workers every draw takes the per-binding path.
- BeginRendering: the render-state memo moves about 2 KB a draw. Zeroing the 592-byte BeginSignature (`rep stos`, :2823) 1.0, copying MakeBeginSignature's result (:2825) 0.7, the compare (:2830) 0.9 plus memcmp 0.24, copying the memo's RenderState out (:2842) 0.4. With MakeBeginSignature 1.6, the memo costs about 4.8 % of B.
- GetBarriers: the first touch of the image backing's state (image.cpp:264–271, :334–341), 1.6, though most calls return without a barrier; the per-subresource loop for views with a mip or layer range (:291–322) 1.0.

Brief. Stage B still sets the pace: busy 84–92 % of the wall (in its handler), stage A waiting 14–26 % at OnSubmit. A change that helps only stage A can shorten a frame by at most B's time outside the handler, 8–16 % of the wall; `top -H` has DrawRec off the CPU 8–17 % in the same windows. Of that, the spin is the 4.4 % of B's samples this profile sees. After 50 µs of spinning (fewer than 12 threads, vk_draw_pipe.h:179–185) B sleeps in `wake.wait` (:192–197), and `cpu-clock:u` cannot see the sleep. The rest of an A-only gain becomes more waiting at OnSubmit. Self shares are upper bounds. (1) **Task 3, 16-word register blocks**, is the only task that takes work off B: RunDrawPacket's block copy is 4.6 % of B, and PostDraw's is 3.4 % of A. Halving the block bytes saves up to about 2.3 % of B and 1.7 % of A, less where both halves of a 32-word block are dirty. It goes first. (2) **Task 2, mask cleared while scanned and the rings as a member**, is stage A only: scan 1.3, clear 1.0, `thread_local` rings 0.4, so ≤ 2.7 % of A. B has no dirty mask (it walks the block list, :782), and B's 2.2 % `__tls_get_addr` comes from the user-data snapshots and `stage_regs`, which the task does not touch. Demoted to after Task 3. (3) **Task 4, no checksum without workers**, is stage A only, 2.9 % (HashRegisterPacket); nothing on B. (4) **Tasks 5–6, constant-ring copies**: stage A only. The ceiling is the memmove under CopySparseMemory, 4.8 % of A in this slice (6.5 over 15:07:00–15:07:20), times the repeat rate. B binds the ring offset through FindRingBinding (vk_rasterizer.cpp:2207, inside BindBuffers, 2.3 % inclusive) whether A copied the buffer or not, so Task 6 changes nothing on B. Task 5 is a counter and keeps its place before Task 6; Task 6 goes last. None of Tasks 2–6 reaches B's biggest costs. Candidates for a follow-up plan, named and not designed: the BindTextures tree (20.7 % inclusive: GetBarriers through Transit, the per-binding start and memo check, CachedImageDescEntry, TouchImage; the texture-set memo is off without workers); ResolveVertexBuffers (11.6 %: GetVertexInputs 5.2, ObtainBuffer 5.8); the BeginRendering memo (9.6 % inclusive, about 4.8 % zeroing, copying and comparing the signature); PrepareRenderState and UpdateDynamicState (5.7 and 5.2 % inclusive); `__tls_get_addr` (2.2 %); and the attribute-vector copy at :1232–1233 (about 1 %). On stage A, at 0 workers, the pipeline selection is the biggest item (RefreshGraphicsKey 44 % inclusive), but A is not the bound.

## Per-draw changes (2026-11-03)

| change | pack start | screenshot same |
|---|---|---|
| 16-word register blocks (Tasks 2–4, 57eab26): after, the 16-word build; before, 32-word blocks in the Task 2 build (CPU experiments, workers off) and in today's baseline (108143a) | after vs Task 2 build: 47.2 vs 50.8 FPS at 4186 vs 4001 draws (4.94 vs 4.74 µs per draw, B busy 95.1 vs 93.8 %); after vs baseline: 56.1 vs 50.2 and 58.3 at 3222 vs 3298 and 3107 (5.33 vs 5.46 and 5.19 µs, B 90.1 and 92.2 %) | yes: the grid at 0.00.0 (15:07:02.938 vs 15:46:38.924) and the launch at 0.05.0 vs 0.04.2 (15:07:17.933 vs 15:46:43.927) |
| Tasks 2–5 (adds Task 5's ring counter; the checksum skip is already in the Task 3 build), f53bd0d: after, this build, with Task 5's `ring copies` counter on (`GOW3_FRAME_STATS=1`), which costs a set insert per constant-ring copy and which every measured build from this one on carries; before, the Task 3 row's race (57eab26) and the Task 2 build (CPU experiments, workers off) | after vs Task 3 race: 45.0 vs 47.2 FPS at 4248 vs 4186 draws (B busy 85.5 vs 95.1 %, stage A waited 9.5 vs 20.7 %), 56.7 vs 56.1 at 3323 vs 3222 (B 88.8 %, not given for 3222); after vs Task 2 build: 45.0 vs 50.8 at 4248 vs 4001 (B 85.5 vs 93.8 %, A waited 9.5 vs 19.6 %); the launch has no Task 2 partner | yes: the grid at 0.00.0 (15:46:38.924 vs 15:50:45.614) and the launch at 0.04.2 (15:46:43.927 vs 15:50:50.610) |
| Constant-ring copy once per submission (Tasks 2–6, 5afc591): after, this build, with the `ring copies` counter on as in the row above; before, the Tasks 2–5 row's race (f53bd0d) and the Task 2 build (CPU experiments, workers off). The plan's rule: kept only if `ring copies` per frame fall by at least 15 % against the Task 5 build, compared per draw at matched windows | after vs Task 5 race: 45.9 vs 45.0 FPS at 4150 vs 4248 draws (B busy 84.7 vs 85.5 %, stage A waited 8.9 vs 9.5 %), 58.4 vs 56.7 at 3105 vs 3323 (B 88.4 vs 88.8 %, A waited 14.3 vs 13.7 %); after vs Task 2 build: 45.9 vs 50.8 at 4150 vs 4001 (B 84.7 vs 93.8 %, A waited 8.9 vs 19.6 %), 58.4 vs 59.3 at 3105 vs 2879 (B not given for 2879). Copies per draw against the Task 5 race: 2.29 vs 2.68 at the grid (−14.7 %), 2.07 vs 2.51 at the launch (−17.7 %), −15.4 % mean over 22 race windows (−10.6 to −19.5 %): the grid misses 15 %, the launch and the mean pass | yes: the grid at 0.00.0 (15:50:45.614 vs 16:32:24.658) and the launch at 0.04.2 vs 0.04.1 (15:50:50.610 vs 16:32:29.648) |

Verdict: kept, inconclusive. One race cannot resolve the change. The Stage B brief caps Task 3 at 2.3 % of B and 1.7 % of A, about 1 FPS at 47 FPS; Tasks 2 and 4 are stage A only and shorten a frame only as far as B keeps up. The race-to-race spread at matched draws in the CPU experiments is 3.5–5.8 FPS. Against the Task 2 build there is one pair: the after side is 3.6 FPS (7 %) lower at 4.6 % more draws, 197.6K draws per second against 203.3K, 2.8 % fewer. That is at the low end of the spread, so one race does not tell it from noise either way; a real 7 % loss would also be three times the 2.3 % of B the change touches, with B setting the pace. Against the baseline, 56.1 at 3222 is inside the before side's 50.2–58.3 at 3107–3298. The verdict stands on the FPS spread alone: stage A's µs per draw in the row (the `GPU thread` figure) is about one over the draws per second (see the Tasks 2–5 row below), so it carries nothing beyond the FPS. Not shown slower, so the constant stays.

One Vineta K race on the 16-word build, GitHub run 41664959462 at commit 57eab26 (run 20261103-154540, `draw preparation workers: 0 (8 hardware threads)`), against the Task 2 build's workers-off race (run 20261103-112741, `GOW3_PREP_WORKERS=0` on f174520) and today's baseline race (run 20261103-150434, 108143a; the code of the two before builds differs only in the worker default). 57eab26 has Task 2 (770f209) and Task 4 (134739a) under it, and no 32-word build with Tasks 2 and 4 was raced: the row measures the three together. Windows are the 5 s `Frame stats` windows at ≥ 3000 draws, paired within ±10 % of the before side's draws. B busy is from the `Draw pipe` line in the same stdout block; that line has its own 5 s timer, so its window is near the frame stats window, not the same. The 3222 window has none in its block (the next comes at 15:46:50.380), so its B busy is not given.

Left out. After: 3412 at 45.2 (worst frame 342.9 ms, the load into the race). Task 2 build: 2631 at 40.1 (worst 586.3 ms, the load) and the next window, 3795 at 50.1 (worst 79.6 ms, five frames' time in one frame right after the load; left out as a possible stall, like the baseline's 143 and 122 ms windows, while 43 ms and under is kept). 3795 would also be 10.3 % under 4186, outside ±10 %, so it is not a pair either way. Baseline: 3026 at 53.4 and 2851 at 53.0 (worst 143.4 and 121.9 ms), the "START RACE" flyover before the grid, which the after race skipped. Not pairs: 3744 at 49.1, the baseline's grid window, is 10.6 % under 4186; the after side's launch window (3222) has no launch partner in the Task 2 build (2879 at 59.3 is 10.6 % under) and pairs by draws only with a later-lap 3073 at 59.8, at the cap (−3.7 FPS for after). Later laps at ≥ 3000 draws are at or near the 60 FPS cap and prove nothing: after 60.0 at 3245 and 3100; Task 2 build 59.8 at 3073; baseline 56.0 at 3240, 56.7 at 3027, 59.3 at 3346, 59.4 at 3089.

The baseline's pack start favours after: `perf-start.sh` sampled both pipeline threads at 1999 Hz with `-g` over 15:06:57.4–15:07:37.4, which covers all its pack-start windows, and the race was paused on the grid (screenshot 15:07:07.933, the pause menu). Race length: 57 `Frame stats` lines before against 34 after. Most of the gap is before the race: 15 windows of boot and menus against 8, the title-screen attract (15:06:05–15:06:27, "PRESS ANY BUTTON TO START"), the flyover and the pause. From the first pack-start window to the exit it is 29 windows against 25, 2:20 against 2:00.

Screenshots (`GOW3_SCREENSHOT_EVERY=5`), the same place in both races: the grid with the HUD up at 0.00.0, lap 1, 8th of 8, and the launch at 442–447 km/h under the orange and yellow boards. The same scene, geometry and colours, no flicker. The differences are timing: the countdown board (blank against red lights; "GO" at 15:07:12 in the baseline), the AI ships to the left and ahead, and the sun's glare on the launch shot.

Tasks 2–5 row. Verdict: inconclusive; nothing reverted. Against the Task 3 race the code differs only by Task 5's counter (6fdcd30), so that pair measures the counter and noise. The grid window is 2.2 FPS (4.7 %) lower at 1.5 % more draws, 191.2K draws per second against 197.6K (3.2 % fewer), and the launch window is 0.6 FPS higher at 3.1 % more draws, 188.4K against 180.8K (4.2 % more). The two straddle zero, inside the 3.5–5.8 FPS race-to-race spread. Against the Task 2 build, which this build leads by Tasks 2–5 together, the grid window is 5.8 FPS (11 %) lower at 6.2 % more draws, 191.2K against 203.3K draws per second (5.9 % fewer). That is the top of the spread, and the second race since the Task 2 build that is lower in that window (the Task 3 row's 47.2 was 3.6 FPS lower): it is not a gain, and one race does not tell it from noise. The grid window is also the weakest one kept: its worst frame, 42.9 ms, is at the 43 ms limit, and it is the window after the 1.4 s load stall, the place of the Task 2 build's left-out 3795. In its block's `Draw pipe` line stage A waited 9.5 % at OnSubmit against 20.7 and 19.6 % in the two before races, and stage B was busy 85.5 % against 95.1 and 93.8 %. Stage A set more of the pace than before, which is what a cost on stage A such as the counter would do; that line has its own 5 s timer, so this is suggestive, not proof. Stage A's µs per draw is left out of this row: it is the thread's user + sys time per draw, A's spins and yields at OnSubmit count in it, and with A 0.1 % idle it is about one over the draws per second (5.08 µs × 191.2K = 0.97 s a second), so it follows the FPS.

The counter's cost. With `GOW3_FRAME_STATS=1` every constant-ring copy through CopySparseMemory does an `unordered_set` insert on stage A: 11387 a frame at the grid, about 0.51 M a second at 45 FPS, 2.7 a draw. About two thirds of the keys are new in each submission (34 % repeats), and each new key also allocates a node, freed by `clear()` when the submission is retired. It is not measured separately: against the Task 3 race it is the only code difference, and the pairs above do not resolve it. Every measured build from this one on carries it, so later rows compare like with like against this race, and carry this cost against the Task 2 build and the Task 3 race.

Constant-ring repeats (Task 5), from `ring copies N/frame, repeats M/frame` at the end of each `Frame stats` line. A repeat is a copy whose (address, size) was already copied in the same submission; Flatbuf copies (the `source` path) and failed allocations are not counted. At the pack start: ring copies 11387/frame, repeats 3875/frame at 4248 draws, 34.0 %; 8343 and 2920 at 3323 draws, 35.0 %; 9501 and 3332 (35.1 %) in the left-out load window. Every race window from the load to the finish is 33.0–35.1 % (1207–4248 draws, 2.2–2.7 copies a draw); the menus before the race are 0–23 %. Task 6 proceeds: 34–35 % is above the 25 % threshold. Its ceiling is on stage A only: the constant-ring memmove under CopySparseMemory, 4.8 % of A in the Stage B slice (6.5 over 15:07:00–15:07:20, both at 108143a), times 0.34–0.35, about 1.6–2.3 % of A. Stage B, which sets the pace, binds the ring offset either way.

One Vineta K race on GitHub run 41664881062 at commit f53bd0d (run 20261103-154926, `draw preparation workers: 0 (8 hardware threads)`); the code is Tasks 2, 4, 3 and 5, and the commits above 6fdcd30 are notes. Before: the Task 3 row's race (run 20261103-154540, 57eab26) and the Task 2 build's workers-off race (run 20261103-112741). The method is the row above's: 5 s `Frame stats` windows at ≥ 3000 draws, paired within ±10 % of the before side's draws, worst frame 43 ms or less, B busy and A's wait from the `Draw pipe` line in the same stdout block. Stamps lag their windows by 2.5–4 s: the grid window is stamped 15:50:50.980, the launch window 15:50:53.438.

Left out. After: 3641 at 29.9 (worst 1425.9 ms, the load into the race). Before: the windows the row above leaves out (the Task 3 race's 3412, the load; the Task 2 build's 2631 and 3795). Not pairs: the launch window (3323) has no launch partner in the Task 2 build (it is 15.4 % over 2879 at 59.3) and pairs by draws only with later-lap windows, 3593 at 56.2 (+0.5 FPS for after) and 3073 at 59.8, at the cap (−3.1). Later laps at ≥ 3000 draws are near the 60 FPS cap and prove nothing: after 57.4 at 3383 (15:51:26.067, the start of lap 2: lap 1 ended at 0.36.76, screenshot 15:51:25.599) and 58.2 at 3080 (15:52:38.372). By draws they pair with the Task 3 race's 60.0 at 3245 and 3100 (−2.6 and −1.8 FPS for after) and with the Task 2 build's 56.2 at 3593 and 59.8 at 3073 (+1.2 and −1.6). The before builds dip under the cap on later laps too (Task 2 build 56.2, baseline 56.0 and 56.7), so these are mixed.

Screenshots, the same place in both races (Task 3 race against this one): the grid with the HUD up at 0.00.0, lap 1, 8th of 8, `CUSA07671_20261103_154638_924.png` against `CUSA07671_20261103_155045_614.png`; the launch at 0.04.2, 7th of 8, 442 against 446 km/h, `CUSA07671_20261103_154643_927.png` against `CUSA07671_20261103_155050_610.png`. The same scene, geometry and colours, no flicker. The differences are timing: the countdown board's number, the AI ships, the ship's line and the camera's angle at the launch (more of the tower on the left in this race), and the energy readout, blank in this race's launch shot and 99 in the next one (15:50:55.599), so it was changing.

Tasks 2–6 row. Verdict: on the 15 % line, which one race cannot settle; nothing reverted here. Copies per draw fell 14.7 % at the grid and 17.7 % at the launch, 15.4 % on average over every matched race window (median 15.3 %, 12 of 22 pairs at 15 % or more), and 15.3 % pooled over all kept race windows (2.11 against 2.49 copies a draw). The pairs' standard deviation is 2.6 points, and the 22 pairs share 13 Task 5 windows, so they are not independent: 15 % is inside the noise of this measurement. Per frame the falls are larger, 9486 against 11387 (−16.7 %) at the grid and 6417 against 8343 (−23.1 %) at the launch, but the after windows have 2.3 and 6.6 % fewer draws; per draw is the figure that decides. The pack start is not slower than the Task 5 race: the grid window is 0.9 FPS higher at 2.3 % fewer draws, 190.5K draws per second against 191.2K (0.4 % fewer), and the launch window 1.7 FPS higher at 6.6 % fewer draws, 181.3K against 188.4K (3.8 % fewer), both inside the 3.5–5.8 FPS race-to-race spread. Nothing more was expected: the cache saves stage A at most the constant-ring memmove (4.8 % of A in the Stage B slice, 6.5 over 15:07:00–15:07:20) times the 15 % of copies it removes, 0.7–1.0 % of A, before its own lookups, node allocations and clears, and stage B, which sets the pace, binds the ring offset either way. Against the Task 2 build the grid window is 4.9 FPS (9.6 %) lower at 3.7 % more draws, 190.5K draws per second against 203.3K (6.3 % fewer). That is the third build in a row below the Task 2 build's 50.8 there (−3.6, −5.8 and −4.9 FPS): the Task 2 race may have been a high one, or Tasks 2–6 or the counter may cost something at the grid; one race per build does not tell them apart. The launch now pairs with the Task 2 build: 58.4 against 59.3 FPS at 7.8 % more draws, 181.3K against 170.7K draws per second, both near the cap, so it says little. This grid window is cleaner than the Task 5 race's: worst frame 29.7 ms against 42.9.

Copy rate. Each kept window of this race from the grid to the finish is paired with the kept Task 5 race window nearest in draws within ±10 % of the before side's draws, the grid with the grid and the launch with the launch. Copy counts do not depend on the FPS cap (a frame's copies are what its draws bind), so the capped mid-lap windows are valid here, though they prove nothing for FPS.

| windows | pairs | draws, after | copies per draw, after | copies per draw, Task 5 race | fall per draw, mean | range |
|---|---|---|---|---|---|---|
| grid | 1 | 4150 (vs 4248) | 2.29 | 2.68 | 14.7 % | |
| launch | 1 | 3105 (vs 3323) | 2.07 | 2.51 | 17.7 % | |
| later, ≥ 3000 (the lap lines, the finish) | 3 | 3180–3675 | 2.19–2.20 | 2.51–2.60 | 14.8 % | 12.5–15.9 % |
| 2000–2999 | 7 | 2107–2922 | 2.05–2.18 | 2.33–2.61 | 16.2 % | 10.6–18.9 % |
| under 2000 | 10 | 1564–1987 | 1.96–2.07 | 2.31–2.44 | 14.8 % | 10.6–19.5 % |
| all race windows | 22 | 1564–4150 | 1.96–2.29 | 2.31–2.68 | 15.4 % | 10.6–19.5 % |

The noise is the before side's own scatter: copies per draw rise with draws in both races, and Task 5 windows at nearly the same draws differ by up to 0.1 a draw (2.37 at 2005, 2.33 at 2020, 2.43 at 1915), about 4 %. Taking every pair within ±10 % instead of the nearest one (84 pairs) gives 15.4 % mean, 10.1–21.3 %. Adding the stall-like mid-lap windows on both sides leaves the mean at 15.4 %.

`ring_repeats` now counts cache hits too (a hit is a repeat, and so is a range copied again after a clear), so the repeat share, 39.8 % of copies at the grid and 42.0 % at the launch, is not Task 5's 33–35 % repeat rate and does not compare with it; copies per draw does. Repeats per draw do compare: 0.911 against 0.912 at the grid, 0.868 against 0.879 at the launch, 0.998 of Task 5's on average over the 22 pairs. The same ranges repeat in both races, so this build's copies plus hits are Task 5's copies, and the fall is the hits: about 1640 a frame at the grid, 43 % of its 3779 repeats, and 1380 at the launch, 51 % of 2695. The rest are copied again after one of the cache's clears. Where nothing clears, in the menus at 126 draws, copies fell from 224 to 172 a frame with 52 repeats in both: every repeat hit (−23.2 %). The brief expected copies to fall by the repeat rate, 34–35 %; in the race they fell by less than half of that.

Task 6: not applied (reverted, 979f2dc and b2b56c1). This replaces the row's verdict above. The row's rule was met: copies per draw fell 15.4 % on average, and per frame both pack-start windows fell by more than 15 % (−16.7 % at the grid, −23.1 % at the launch); only the grid's per-draw window, at −14.7 %, missed it. The fall is inside this race's noise (pairs' standard deviation 2.6 points). The revert rests on two other grounds. First, the plan's own acceptance criterion for Task 6, that copies fall by about the Task 5 repeat rate (34–35 %), was missed: they fell about 15 %. Second, the gain did not pay for the cost. The frame rate did not move: both pack-start windows are within 1.7 FPS of the Task 5 race, inside the 3.5–5.8 FPS race-to-race spread. Nothing more was possible: the ceiling is about 1 % of stage A (0.7–1.0 %), and stage B sets the pace. The cost was a lifetime rule on the ring, `ConstantRing::Hold` and `RingPositions`, so that a range a later packet reuses is not retired while that packet is pending on the GPU, and twelve `InvalidateRingCache` calls that clear the whole cache (six in liverpool.cpp, six in vk_rasterizer.cpp), each one a place a later change must keep right. The cost came without a gain. The code under `gpu` and `tests` is back to 6fdcd30, and ring-cache-test and constant-ring-test are gone from gpu/CMakeLists.txt. Task 5's counter stays. It costs nothing with `GOW3_FRAME_STATS` off and a set insert per copy with it on, and its `ring copies` and `repeats` are what measured the 34–35 % repeats and the 15.4 % fall, which is what made this decision possible. With the cache gone, `ring_repeats` again counts Task 5's repeats only, not repeats plus hits. The 0 % speed bar at 433 km/h (`CUSA07671_20261103_163239_648.png`, below) is unexplained and was not seen in any other shot. It is not counted against the cache, and with the code reverted it no longer bears on it. A follow-up, not done: only 43–51 % of repeats became hits, the rest copied again after a clear, and a clear counter per call site would show which clears cost the hits, and so how far the hit rate could rise.

One Vineta K race on GitHub run 41672973213 at commit 5afc591 (run 20261103-163100, `draw preparation workers: 0 (8 hardware threads)`); the code is Tasks 2, 4, 3, 5 and 6 (8b37868, 5afc591). Before: the Tasks 2–5 row's race (run 20261103-154926, f53bd0d) and the Task 2 build's workers-off race (run 20261103-112741). The method is the rows above': 5 s `Frame stats` windows at ≥ 3000 draws for FPS, paired within ±10 % of the before side's draws, worst frame 43 ms or less, B busy and A's wait from the `Draw pipe` line in the same stdout block (the Task 2 build's 2879 block has none). Stamps lag their windows by 2.5–4 s: the grid window is stamped 16:32:28.513, the launch window 16:32:35.189.

Left out. After: 2833 at 40.2 (worst 854.2 ms, 16:32:18.062, the load into the race) and 3570 at 50.9 (worst 146.9 ms, 16:32:24.704, the "START RACE" flyover after the load, screenshot 16:32:19.647); mid-lap 1897 at 60.0 (47.3 ms, 16:32:44.371) and 1736 at 58.8 (98.6 ms, 16:33:22.236). Before, for the copy rate as well: the Task 5 race's 3641 (the load), 1679 at 55.7 (346.0 ms) and 1588 at 59.2 (98.0 ms); the Task 2 build's 2631 and 3795 as in the rows above. Later laps at ≥ 3000 draws are near the 60 FPS cap and prove nothing: after 55.9 at 3675 (16:33:05.717, at the lap line: lap 1 ended at 0.37.64, screenshot 16:33:04.648), 57.5 at 3311 (16:33:37.307, at the lap line: lap 2 ended at 0.35.01, screenshot 16:33:39.650) and 57.5 at 3180 (16:34:14.912, the finish, screenshot 16:34:09.650 at 0.31.5). By draws they pair with the Task 5 race's 57.4 at 3383 (−1.5 FPS for after against 3675, +0.1 against 3311 and 3180) and 58.2 at 3080 (−0.7 against 3311 and 3180); 3311 is 11.4 % over the Task 5 race's lap-line window, 2971 at 59.8.

Screenshots, the same place in both races (Task 5 race against this one): the grid with the HUD up at 0.00.0, lap 1, 8th of 8, `CUSA07671_20261103_155045_614.png` against `CUSA07671_20261103_163224_658.png`; the launch at 0.04.2 against 0.04.1, 7th of 8, 446 against 445 km/h, `CUSA07671_20261103_155050_610.png` against `CUSA07671_20261103_163229_648.png`. The same scene, geometry and colours, no flicker. The differences are timing: the AI ships beside and ahead, the ship's line and the camera's angle at the launch (more of the buildings on the right in this race), and the energy readout, 99 in this race's launch shot and blank in the Task 5 race's. All 39 shots of this race (16:31:09.649–16:34:19.646: boot, menus, loading, the flyover, three laps, the replay, the results) show no wrong colours, flicker or misplaced geometry; one HUD readout is unexplained (below). The player's Feisar looks yellow and teal leaving the tunnels (0.19.1 in lap 1, 0.11.4 and 0.11.5 in laps 2 and 3) and dark elsewhere; the Task 5 race shows the same at 0.13.9, 0.18.8 and lap 2's 0.22.0, so it is the track's lighting, not this build. One shot is unexplained. At 0.14.1 (`CUSA07671_20261103_163239_648.png`, 16:32:39.648) the camera is turning fast and the whole frame is smeared, with a white smear at the top right, and the bar under the speed reads 0 % and is empty at 433 km/h. Every other shot at speed in both races reads 100 %; the bar reads 0 % only on the grid, in both races. The Task 5 race's shot at 0.13.9 has the ship level, so it is not a partner. Most likely it is input: the label and the bar agree, and the cache is cleared every submission, so a stale range could only hold bytes from earlier in the same frame. But no second shot shows it. The replay after the finish has the same orange haze in both races (16:34:14.650 against 15:52:35.601).

### main against this branch, same session (2026-11-03)

| build | pack start | 2500–3000 draws | screenshot same |
|---|---|---|---|
| main (1f38fa8, GitHub run 41681702013: 4 draw-preparation workers and the scanner, `gow3:DrawPrep0–3` and `gow3:DrawScan` running) against this branch (Tasks 2–5 code, GitHub run 41664881062 at f53bd0d, 0 workers; the code under gpu/, tests/ and tools/ at the branch head is the same) | 41.0 against 53.2 FPS at 3811 against 3942 draws (+3.4 %; 156.3K against 209.7K draws/s, +34 %); 51.0 against 57.9 and 53.4 at 3484 against 3401 and 3377 (−2.4 and −3.1 %); 53.6 against 57.8 and 58.7 at 3270 against 3237 and 3242 (−1.0 and −0.9 %) | 56.6–58.1 against 58.9–60.3 (near the 60 FPS cap) | not compared |

Two Vineta K races back to back, main first (run 20261103-172301), then this branch (run 20261103-172714), the same gow3.ini for both: upscaler TAA, render resolution 720p, output 1280x720, `exp-run.sh` with no arguments. Pairs are within ±10 % of main's draws. Left out, as stall-like (worst frame above 43 ms): main 4253 at 26.9 (1382.6 ms) and 3329 at 46.5 (50.7 ms); branch 4206 at 36.9 (519.9 ms) and 3545 at 47.9 (147.0 ms); in both races these are the load into the race and the window after it. Below 2500 draws both builds are at the cap (59–60). The 3811/3942 pair matches in draws, not in place: the branch's window is about 30 s later in the race.

Verdict: at the heaviest pack start main gives 41 FPS and this branch 53, +12 FPS, the same as the workers-off experiment's +10–13 (CPU experiments), so most of the branch's gain over main is "no workers on ≤ 8 hardware threads" (0ce0778); the per-draw changes of this plan cannot be told apart in these numbers. From 3200 to 3500 draws the branch is 2–7 FPS ahead; mid-race both are at or near the cap. One race per build, 3.5–5.8 FPS of race-to-race noise.
