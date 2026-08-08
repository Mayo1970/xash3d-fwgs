# AGENTS.md - xashPS3 porting bible

## 1. What this is

A PlayStation 3 homebrew port of [Xash3D-FWGS](https://github.com/FWGS/xash3d-fwgs)
(GoldSrc-compatible engine), built against the open-source **PSL1GHT /
ps3toolchain** stack only -- never the official Sony SDK. The engine source
here is a full vendored copy (not a submodule), matching how the other
console ports in this workspace (ioQuake3-PS4, ioQ3-One) are structured.
Upstream `xash3d-fwgs` (sibling directory) is read-only reference material,
not edited from here.

Testing is **real PS3 hardware only** (CFW/HEN). All hardware validation
below assumes the UDP-log-and-console workflow.

## 2. Hardware validation protocol

This project follows a strict goal-oriented, hardware-validation-gated
workflow. Do not implement more than one goal-stack item ahead. Do not
speculate on hardware behavior -- verify on the actual console.

- **ANALYSIS** -> **IMPLEMENTATION** -> **BUILD_REQUEST** -> **WAITING_FOR_HARDWARE** -> **VALIDATION** -> **NEXT_GOAL**
- `BUILD_REQUEST` must give: exact build command, expected output
  (`EBOOT.BIN`/`.pkg`), deploy method (FTP via webMAN, or PKG install from
  USB), and what should be observed on screen/audio/controller.
- `WAITING_FOR_HARDWARE` stops all speculation until the user reports back
  exactly `SUCCESS: ...` or `FAILURE: ...`.
- On `FAILURE`, produce a minimal isolated test case or a 3-item diagnostic
  checklist -- never rewrite the whole implementation and guess again at the
  same time.
- If the same goal fails hardware validation more than twice in a row: stop,
  escalate (UDP register trace, webMAN/ps3mapi memory peek, or community
  consultation), and ask whether to mark the goal BLOCKED or continue with
  new diagnostic data.

## 3. Goal stack

- [x] **0. Scaffolding** (this session) -- repo structure, build-system
      wiring, platform stub skeleton. No build attempted yet.
- [x] **1. Toolchain bring-up**: null `main()` -> fself -> pkg -> boots to a
      black screen on real hardware, with the UDP debug log sink
      (`nc -ul 18194`) wired up first, before anything else. **VALIDATED on
      real hardware 2026-07-19** via `tools/ps3_bringup` built inside the
      `ps3dev/ps3dev:latest` Docker image (native sfo/pkg tools, no pyexpat
      issue) -- black screen, no crash/XMB-return, UDP heartbeat confirmed
      (also confirmed `sizeof(void*)==8`, `sizeof(long)==8` on real hardware,
      matching the LP64 finding in section 6).
- [x] **2. Platform stubs compile**: `./waf configure --ps3 && ./waf build`
      reaches the link stage. **DONE 2026-07-19** -- `engine/xash` links as a
      real ELF64 big-endian PowerPC64 EXEC inside the `ps3dev/ps3dev:latest`
      Docker image (the project's canonical toolchain). Numerous real,
      build-verified fixes landed along the way (see AGENTS.md section 6 and
      project memory for the full list -- wrong toolchain-header assumptions,
      missing libc functions, a GNU ld static-archive ordering bug, etc.).
      **Follow-up RESOLVED 2026-07-19 (build-verified, not yet hardware-
      validated)**: `filesystem_stdio`/`ref_soft` now build via
      `--static-linking=filesystem_stdio,ref_soft` (`ref/soft/exports.txt`
      added, `GetRefAPI`). Three real bugs found and fixed getting there,
      all in the generic `scripts/waifulib/xshlib.py`/`xcompile.py` tooling,
      none PS3-specific hacks:
      1. `xcompile.py`'s `PS3` class had no `ld()`/`objcopy()` methods and
         never pointed `conf.environ['LD']`/`['OBJCOPY']` at the cross
         binutils (PSP's block already does this) -- `xshlib.py`'s
         `conf.find_program('ld'/'objcopy')` fell back to the host's.
      2. Even after adding those, top-level `wscript`'s
         `conf.load('xshlib xcompile ...')` loaded `xshlib` *before*
         `xcompile`, so the environ overrides didn't exist yet when
         `xshlib.configure()` ran `find_program`. Fixed by reordering to
         `conf.load('xcompile xshlib ...')`.
      3. With the real cross `ld -r` in place, host `/usr/bin/ld` had been
         silently mangling `filesystem/VFileSystem009.cpp`'s PPC64 ELFv1
         `.opd`/`R_PPC64_TOC` relocations (the only C++ TU in
         `filesystem/`) -- `ld: Relocations in generic ELF (EM: 21)` /
         `error adding symbols: file in wrong format`. Root cause was
         purely #2; once real `ld` was used this was a non-issue. Separately,
         `xshlib`'s `ld -r` task didn't link each relocatable module's own
         STLIB `use` deps (e.g. `ref_soft` -> `ref_common`), so `Matrix4x4_*`
         /`gEngfuncs`/etc were undefined at `xash`'s final link; fixed by
         adding `${STLIBPATH_ST:STLIBPATH} ${STLIB_ST:STLIB}
         ${LIBPATH_ST:LIBPATH} ${LIB_ST:LIB}` to `xshlib`'s `run_str` (bare
         `ld`, not gcc, so no `-Wl,`/`STLIB_MARKER` wrapping) -- this lets
         `ld -r`'s normal archive-member selection pull `ref_context.c.o`
         into `ref_soft.o`, where the existing `objcopy -G
         lib_ref_soft_exports` step then localizes those symbols so they
         don't collide with the engine's own identically-named globals
         (each ref module is designed to carry its own private copy of
         these, normally isolated by being a separate `.so`).
      Full chain (`./waf configure --ps3
      --static-linking=filesystem_stdio,ref_soft --disable-mbedtls && ./waf
      build`) now reaches a real ELF64 big-endian PowerPC64 EXEC ->
      stripped -> sprxlinked -> `EBOOT.BIN` -> `EBOOT.pkg`, inside
      `ps3dev/ps3dev:latest` Docker. **VALIDATED on real hardware
      2026-07-19** -- three real, build/hardware-verified bugs found and
      fixed getting from "installs but 0x80010006 on boot" to a clean run
      through real engine init (see AGENTS.md section 6 and project memory
      for full detail):
      1. `scripts/waifulib/ps3.py`'s `apply_pkg` pointed `PKGDIR` at the raw
         build directory (bundling every intermediate object file/ELF -- a
         2MB EBOOT.BIN produced a 63MB pkg) and declared `EBOOT.BIN`/
         `PARAM.SFO` as flat siblings with no `USRDIR/` nesting. LV2 looks
         for `USRDIR/EBOOT.BIN` specifically at boot; installing fine but
         booting with `0x80010006` (ENOENT) is the direct symptom of this
         layout bug, confirmed by diffing against PSL1GHT's own stock
         `ppu_rules` `%.pkg` recipe (identical on both a local ps3dev
         install and the Docker image). Fixed by staging a clean `pkg/`
         dir (`PARAM.SFO` + `ICON0.PNG` at root, `EBOOT.BIN` under
         `USRDIR/`, with a real `cpfile` copy task for the source-tree
         icon) and pointing `PKGDIR` there instead.
      2. The UDP debug log (goal-1's only observability channel) only ever
         had the two explicit `PS3_Printf` calls in `PS3_Init`/`PS3_Shutdown`
         -- none of the engine's own `Con_Printf`/`Sys_Error` output reached
         it, making every hardware failure a source-reading exercise instead
         of a log read. Fixed by adding a `#if XASH_PS3` branch in
         `Sys_PrintStdout()` (`engine/common/sys_con.c`), mirroring the
         existing per-platform precedent there (Android/NSwitch/PSVita
         already do exactly this for their own debug channels). Also had to
         bump `PS3_Printf`'s internal buffer from 1024 to `MAX_PRINT_MSG`
         (8192) since it's now the sink for arbitrary engine console lines,
         not two short fixed strings.
      3. `filesystem/exports.txt` only listed `GetFSAPI`, but
         `filesystem_engine.c:253` separately requires a `CreateInterface`
         entry point (real symbol at `filesystem/VFileSystem009.cpp:506`,
         `extern "C"`). The static-link `objcopy -G lib_filesystem_stdio_exports`
         step localizes everything not explicitly listed in `exports.txt`,
         so `CreateInterface` was hidden along with the module's internals
         -- `FS_LoadProgs` found `GetFSAPI` fine (proving the static-link
         table mechanism itself works) but errored on the second lookup.
         Fixed by adding `CreateInterface` as a second line in
         `filesystem/exports.txt` (`ref_soft` doesn't need this -- confirmed
         via `ref_common.c:550`, it only ever needs the single `GetRefAPI`
         export).
      With all three fixed, the engine now boots cleanly, runs real
      init (`FS_Init`, `FS_LoadProgs`, `FS_LoadGameInfo`, ...), and reaches
      the *expected* wall: no game data shipped yet, so `Couldn't find game
      directory 'valve'` fires and the engine does a clean `Sys_Quit` back
      to XMB. That is goal 3's job, not a bug.
- [x] **3. Filesystem + asset loading, headless**: load core assets from
      `/dev_hdd0/game/XASH10000/USRDIR`, checksum-verify over the UDP log,
      no rendering yet. **DONE 2026-07-19, VALIDATED on real hardware.**
      User FTP'd a loose-file (no `.pak`) `valve/` tree into USRDIR.
      `PS3_VerifyGameAssets()` (`sys_ps3.c`) checksums `liblist.gam`,
      `gfx/conchars`, `gfx.wad` via the engine's existing `CRC32_File`/
      `FS_FileExists` and logs the results over the UDP sink -- all three
      resolved on hardware (`gfx/conchars`'s CRC32 read failed even though
      the file exists, non-fatal, not investigated further -- goal 3's
      diagnostic scope, not a blocker). Engine now runs past FS init into
      renderer/audio/game-DLL bring-up, failing there only for already-
      documented, already-scoped-later reasons (no `ref_soft`->RSX video
      yet = goal 4, no PS3 audio backend = goal 7, no game DLL vendored =
      goal 12) -- clean `Sys_Error`/shutdown, no crash.

      Getting here required finding and fixing **three real, hardware-
      confirmed bugs**, none guessed -- each verified on real hardware
      before moving to the next (full method notes in project memory):
      1. **Empty rootdir**: `FS_DetermineRootDirectory`
         (`filesystem_engine.c`) had no PS3 branch, so it fell into the
         generic `getcwd()` fallback. PS3's LV2 process starts with a
         literal `"/"` cwd (not the app's own USRDIR), and
         `COM_StripDirectorySlash()` collapses that lone `"/"` to `""`
         *after* the emptiness check already passed -- rootdir silently
         became `""`. Fixed with a `#elif XASH_PS3` branch hardcoding the
         real install path, matching the sibling PS4 port's identical
         `Sys_Cwd()` -> `"/app0"` pattern for the same "no real cwd on a
         console" problem.
      2. **No functional relative-path resolution on PS3 at all**:
         confirmed on hardware that `opendir(".")` returns ENOENT even
         immediately after a "successful" `chdir(fs_rootdir)` -- LV2's
         real filesystem syscalls only understand absolute paths, chdir()
         doesn't establish anything they consult. Fixed with
         `PS3_ResolvePath()` (`filesystem/sys.c`), a small PS3-only helper
         that resolves any relative path against `fs_rootdir` by hand,
         applied at every raw `opendir`/`stat`/`open`/`mkdir`/`rename`/
         `remove` call site in `filesystem/sys.c` and `filesystem/io.c`.
      3. **`FI` global-symbol collision (the real blocker, took the
         longest to find)**: engine's `fs_globals_t *FI` (pointer,
         `filesystem_engine.c`) and the filesystem module's `fs_globals_t
         FI` (the actual struct, `filesystem.c`) are both uninitialized
         globals -> GCC 7.2's default `-fcommon` makes both compile to
         COMMON symbols named `FI`. Upstream Xash3D never hits this
         because engine and filesystem module are separate shared
         objects; this project's `--static-linking` mechanism
         (`xshlib.py`, needed because PSL1GHT has no real dlopen) merges
         them into one relocatable object via `ld -r`, and that `ld -r`
         call was missing `-d` (`--define-common`) -- so the module's own
         `FI` stayed COMMON through the merge, and `objcopy -G
         lib_filesystem_stdio_exports` (which localizes everything not in
         `exports.txt`) cannot localize a COMMON symbol, only a defined
         one. Both same-named COMMONs then silently merged into one
         4112-byte allocation at final link: the engine's 8-byte pointer
         variable and the struct's first field (`GameInfo`) became the
         *same memory*. `searchpath.c`'s entirely-legitimate
         `FI.GameInfo = FI.games[i]` (setting the real gameinfo pointer)
         was therefore also overwriting the engine's own `FI` pointer
         variable with a `gameinfo_t*` -- and the engine's next
         `FI->GameInfo` read then double-dereferenced through that
         mistyped value, landing on `gameinfo_t`'s first field
         (`gamefolder`, e.g. literally `"valve"`) instead of a real
         `GameInfo` pointer. Confirmed root cause via `nm` on the actual
         build artifacts (COMMON `FI` in both `.o`s, single merged `FI`
         in the final ELF) before touching any fix. **Fixed in
         `scripts/waifulib/xshlib.py`**: `add_target()` now always adds
         `-d` to `LD_RELOCATABLE_FLAGS`, letting `objcopy -G` correctly
         localize module-private globals as originally designed --
         zero source changes, and it's a systemic fix for the whole
         `--static-linking` mechanism (any future same-named global
         between engine and a statically-linked module), not a
         one-off patch for `FI` specifically. Verified via `nm` (two
         separate `FI` symbols in the final ELF, correct sizes/scopes)
         *before* the confirming hardware run.
      Also fixed along the way, independently real but not the main
      blocker: PSL1GHT's prebuilt `libc.a` `setjmp()`/`longjmp()` were
      compiled with AltiVec and write 448 bytes into what the stock non-
      AltiVec `machine/setjmp.h` sizes as a 256-byte `jmp_buf` (192-byte
      overflow on every call, plus a separate 64-bit-GPR-truncation bug
      via 32-bit `stw`/`lwz`) -- a known toolchain issue on this exact
      devkitPro GCC 7.2 LP64 build, already root-caused and fixed the
      same way in the sibling `ioQuake3-PS3` port. Ported that port's
      fix: `engine/platform/ps3/include/setjmp.h` (512-byte `jmp_buf`
      shim, found first via `-I` ordering in `xcompile.py`'s PS3
      `cflags()`) + `engine/platform/ps3/ps3_setjmp.S` (correct 64-bit
      `std`/`ld` replacement, no AltiVec). Keep this even though it
      wasn't the `FI` bug's cause -- it's a real, separate landmine
      (`longjmp()` is used for real error recovery in `host.c`/
      `host_state.c`) that would otherwise still be live.
- [x] **4. Video bring-up**: `rsxInit` -> `videoConfigure` -> double-buffered
      vsynced clear-color flip loop, zero engine involvement. **DONE
      2026-07-19/20, VALIDATED on real hardware.** Built as a standalone
      tool, `tools/ps3_video/` (same isolation precedent as goal 1's
      `tools/ps3_bringup/`), cycling a solid clear color (red/green/blue/
      white, ~1s each) through a real `rsxInit` -> `videoConfigure` ->
      double-buffered flip loop. Does not touch `vid_ps3.c` -- its
      `GL_SwapBuffers`/`R_ChangeDisplaySettings`/`SW_UnlockBuffer` TODOs
      are still goal 5's job.

      Took 3 hardware rounds to get right, each a real bug found by
      reading either the actual PSL1GHT headers or the sibling
      `E:\...\quake3\Ioquake3-PS3\ioQuake3-PS3` port's hardware-validated
      RSX code (`code/sys/ps3_glimp.c`) -- not guessed:
      1. **Flip-status wait had inverted polarity and wrong position.**
         `gcm_sys.h`'s own doc comment for `gcmGetFlipStatus()`: "return
         zero if no flip occured, nonzero otherwise." First attempt
         checked `!= 0` *before* issuing the flip (as if nonzero meant
         "still pending") instead of `== 0` *after* issuing it -- this
         left the render loop's only CPU-side throttle permanently
         no-op, hammering the 1-frame-old command buffer far faster than
         real vsync. Symptom: UDP heartbeats logged instantly/unpaced.
      2. **`gcmSurface`'s unused MRT color slots (1-3) can't be left
         zeroed**, even though only `colorTarget = GCM_SURFACE_TARGET_0`
         (slot 0) is actually rendered to. Confirmed by diffing directly
         against `ps3_glimp.c`'s `PS3_RSX_SetRenderTarget`, which
         populates `colorLocation[1..3]`/`colorOffset[1..3]` (reusing
         slot 0's offset) /`colorPitch[1..3]` (dummy `64`) unconditionally.
         `rsxSetSurface` is `void` -- a bad surface config here has *no*
         error-return signal, so this silently produced a fully black
         screen while every other RSX call (`rsxInit`, `videoConfigure`,
         `gcmSetDisplayBuffer`, `gcmSetFlip`) reported `ret=0`. This was
         the second hardware round's failure, isolated only by adding
         explicit return-code UDP logging to every checkable call first
         (all came back clean), which by elimination pointed at the one
         call with no return code to check.
      3. **`gcmGetFlipStatus()`/`gcmResetFlipStatus()` polling (the exact
         sequence `rsx.h`'s own "Quick guide to RSX programming" doc
         comment describes) never actually throttled anything on real
         hardware even after fixing #1** -- every verbose log line read
         `waited=0*200us`, meaning the status read back "flip already
         completed" instantly, every single frame. Root-caused by reading
         `ps3_glimp.c` end-to-end (per the user's standing instruction to
         read sibling-port code immediately instead of re-guessing): it
         never calls `gcmGetFlipStatus`/`gcmResetFlipStatus` at all.
         Instead it registers `gcmSetFlipHandler()` and tracks
         `flip_queued`/`flip_completed` counters incremented by that
         callback, blocking in `PS3_RSX_WaitFlips` on
         `(queued - completed) > FB_COUNT - 2`. Rewrote
         `tools/ps3_video`'s flip-wait to this exact callback-driven
         pattern (generalized from their hardcoded 3 buffers to this
         tool's 2) -- fixed on the first retry. Also matched their
         `gcmSetWaitFlip` -> `gcmSetFlip` -> `rsxFlushBuffer` command
         order (WaitFlip *before* SetFlip, opposite of the doc comment's
         stated order) and their `RSX_CB_SIZE`/`RSX_HOST_SIZE` (1MB/32MB
         vs. the doc's 64KB/1MB "default" this project tried first) while
         at it, to remove every remaining difference from proven-working
         code before the third hardware round.

      **Open question for goal 5**: whether `gcmGetFlipStatus()` polling
      is fundamentally unreliable on this toolchain/hardware combo, or
      only unreliable without a `gcmSetFlipHandler` registered first, was
      not isolated further -- goal 5's real `vid_ps3.c` wiring should just
      use the callback-driven pattern from the start rather than
      re-deriving this.
- [x] **5. ref_soft wired to RSX**: `SW_CreateBuffer`/`SW_LockBuffer`/
      `SW_UnlockBuffer` render into the XDR buffer allocated in `vid_ps3.c`,
      transfer to the RSX display buffer, flip. **VALIDATED on real hardware
      2026-07-20**, after goal 6 unblocked reaching `Host_Frame`: real
      console output (Xash logo watermark, console text, background) visibly
      rendered on the TV via `ref_soft`->`SW_UnlockBuffer`->RSX flip, engine
      stayed responsive (clean XMB exit via the PS button). One real bug
      found on this run and fixed: `SW_CreateBuffer`'s `memalign()`-allocated
      `ps3_swbuffer` was never zeroed, so regions `ref_soft`'s console draw
      doesn't repaint every frame showed visible noise/static from the fresh
      allocation's leftover garbage -- fixed with a `memset` right after the
      `memalign` succeeds (RSX-side color buffers don't need the same
      treatment: `SW_UnlockBuffer`'s per-row copy already overwrites their
      *entire* pitch*height extent every frame, regardless of prior
      content). **Fix confirmed on real hardware 2026-07-20**: clean console
      output, no more noise/static. All-new logic lives in
      `engine/platform/ps3/vid_ps3.c` only, reusing goal 4's hardware-
      validated `tools/ps3_video` pattern exactly (same `RSX_CB_SIZE`=1MB/
      `RSX_HOST_SIZE`=32MB/`FB_COUNT`=2 constants, same
      `gcmSetFlipHandler`-driven flip-completion counters instead of
      `gcmGetFlipStatus` polling, same `gcmSetWaitFlip`->`gcmSetFlip`->
      `rsxFlushBuffer` order). RSX is used presentation-only here -- ref_soft
      never issues an RSX draw command, so unlike goal 4's tool this needs no
      `rsxSetSurface`/depth buffer/render-target at all, only
      `gcmSetDisplayBuffer` + flip.
      - `PS3_RSX_Init()` (new, idempotent) now does the real
        `rsxInit`->`videoGetState`->`videoGetResolution`->`videoConfigure`
        sequence that was previously a TODO; it also fixes a design bug from
        goal 0/4 scaffolding where `vid_ps3.c` hardcoded 1280x720 instead of
        honoring the TV's actual reported mode (the ps3 skill's own failure-
        mode table warns this produces a stretched/squashed image).
        `R_ChangeDisplaySettings` calls it once, then reports the real
        queried resolution to `R_SaveVideoMode` so `ref_soft`'s subsequent
        `SW_CreateBuffer` request matches the RSX buffers' true dimensions.
      - `SW_UnlockBuffer` does a per-row `memcpy` from `ps3_swbuffer` (XDR,
        tightly packed) into the current RSX color buffer (GDDR3, 64-byte-
        aligned pitch) -- row-by-row rather than one flat copy since the two
        pitches are only guaranteed equal at common HD widths, not assumed.
        `ps3_swbuffer`'s ARGB8888 byte layout already matches
        `GCM_SURFACE_A8R8G8B8`'s big-endian byte order (A,R,G,B) confirmed
        against the sibling `ioQuake3-PS3` port's own comment on this exact
        point, so no pixel format conversion is needed, only the copy. A
        `sync` barrier follows the copy before `GL_SwapBuffers()` flips, per
        this platform's documented PPE-store-to-RSX-doorbell ordering
        requirement.
      - Found and fixed one real, pre-existing bug in `SW_CreateBuffer`
        (already "done" from goal 0 scaffolding, not touched by goals 1-4):
        it set `*stride = width * 4` (byte pitch) where every other platform
        in this codebase (`vid_fbdev.c`, `vid_sdl2.c`, `vid_dos.c`) sets it
        in pixel units, confirmed by how `ref/soft/r_glblit.c` indexes the
        locked buffer (`pbuf[stride*v+u]` on a `u32*`) -- would have stridden
        4x too far per row once real pixels were written. Fixed to
        `*stride = width`.
      - Build hit one real toolchain issue, not a logic bug: including
        `<rsx/rsx.h>`/`<rsx/gcm_sys.h>` anywhere in the engine (no prior file
        did) trips this project's project-wide `-Werror=strict-prototypes`,
        because PSL1GHT's `rsx/mm.h`/`rsx/gcm_sys.h` declare several
        functions as `foo()` instead of `foo(void)`. Fixed with the exact
        same `#pragma GCC diagnostic push/ignored "-Wstrict-prototypes"/pop`
        wrap already used by this project's own `in_ps3.c` (`io/pad.h`) and
        `sys_ps3.c` (`net/net.h`) for the identical vendor-header issue --
        not a new pattern.
      Full chain (`./waf configure --ps3
      --static-linking=filesystem_stdio,ref_soft --disable-mbedtls && ./waf
      build`) reaches a real ELF64 big-endian PowerPC64 EXEC -> stripped ->
      sprxlinked -> `EBOOT.BIN` -> `EBOOT.pkg`, inside `ps3dev/ps3dev:latest`
      Docker -- sane sizes (~2.1MB), not the 63MB packaging-bug size from
      goal 2.

      **Two hardware rounds.** Round 1: clean UDP log through renderer/audio/
      gameui init, clean shutdown back to XMB, but screen stayed solid black
      -- ambiguous, since `SW_CreateBuffer`/`SW_LockBuffer` didn't check
      `ps3_rsx_ready` and nothing logged `PS3_RSX_Init`'s outcome, so a
      silent RSX failure and "RSX fine but never asked to draw" looked
      identical from the log alone. Round 2 added `Con_Printf` diagnostics
      (mirrors to the UDP sink automatically, per goal 3's `Sys_PrintStdout`
      fix) to every `PS3_RSX_Init` step and to `SW_UnlockBuffer`'s entry/skip
      paths -- **fully confirmed the RSX video subsystem itself works on
      real hardware**: `rsxInit ret=0`, real `1920x1080` queried via
      `videoGetState`/`videoGetResolution` (not the old hardcoded 1280x720),
      `videoConfigure ret=0`, both `gcmSetDisplayBuffer` calls `ret=0`,
      `PS3_RSX_Init: ready` reached. But **no `SW_UnlockBuffer` line appeared
      at all** -- proving the function is never called, not that it's
      broken. Root cause (traced to `engine/client/cl_main.c:3797-3821`,
      `CL_Init`): `S_Init()`'s return value is never checked (audio failing
      is harmless there, confirmed by the log), but the very next step,
      `CL_LoadProgs(libpath)` loading the client-side game DLL, IS checked
      and its failure calls `Host_Error`, which tears the whole host down
      **before the engine ever reaches `Host_Frame`** -- i.e. before a
      single frame is ever rendered or flipped. `VID_Init()` (which runs
      `PS3_RSX_Init`) already completed successfully earlier in the same
      `CL_Init` call; the video subsystem was never the problem.

      **Newly discovered, load-bearing blocker, not previously in the goal
      stack**: no goal from here on (rendering, input, audio) can be
      visually or interactively confirmed on real hardware until something
      satisfies `CL_LoadProgs`, because none of them run without reaching
      the main loop -- this isn't specific to goal 5. Reordered the goal
      stack below (new goal 6) to address this directly before continuing.
- [x] **6. Minimal loadable client DLL (stub)**: get `CL_LoadProgs`
      (`engine/client/cl_main.c:3820`) to succeed via this project's
      `--static-linking` mechanism (already proven for `filesystem_stdio`/
      `ref_soft` in goal 2) with a throwaway placeholder client module --
      not real game logic, just enough of the required export/ABI surface
      for `CL_Init` to stop hard-aborting via `Host_Error` and let the
      engine reach `Host_Frame` for the first time. Keep it minimal; the
      real port lands at goal 13 below, built from `hlsdk-portable`. This
      unblocks hardware validation for every remaining goal, so treat it as
      load-bearing infrastructure, not scope creep. **VALIDATED on real
      hardware 2026-07-20**: the `Host_Error`/"can't initialize client" line
      is gone, `CL_Init` completes, and the engine reaches `Host_Frame` and
      runs a steady main loop (confirmed responsive -- user could exit
      cleanly via the PS button/XMB rather than a crash or hang). This is
      also what let goal 5 finally get its real hardware confirmation (see
      its entry above) -- `SW_UnlockBuffer` fired for the first time ever on
      this run.

      New module at `stub/client/` (`wscript`, `exports.txt`, `cl_stub.c`)
      -- reused an existing dead scaffold slot already in top-level
      `wscript` (`Subproject('stub/client', lambda x: x.env.CLIENT)`,
      sitting right before `Subproject('engine')` with the comment "keep
      latest for static linking"), so no top-level `wscript` edit was
      needed at all, just populating the directory. The waf target inside
      is named `client` (directory name is independent of target name, same
      as `ref/soft/wscript` building target `ref_soft`) -- this exact
      string is required because `COM_LoadLibrary("client", ...)` resolves
      it (`engine/common/lib_common.c:229`,
      `COM_GenerateClientLibraryPath("client", ...)`, the default when
      `host.clientlib` isn't overridden).

      `cl_stub.c` exports all 37 names `CL_LoadProgs`'s `cdll_exports[]`
      (`engine/client/dll_int/cl_game.c:57-96`) requires, each with the
      exact signature from `cldll_func_t` (`engine/cdll_exp.h:33-86`) so the
      engine can safely call through the typed pointers later, not just
      satisfy the untyped `COM_GetProcAddress` lookup -- `Initialize`
      unconditionally `return 1` (the one hard requirement, `CL_LoadProgs`
      treats 0 as failure), everything else a safe no-op with defensively-
      zeroed output pointers. The 11 `cdll_new_exports[]` (SDK 2.3+
      extensions) are optional and were skipped -- missing ones only log a
      warning, confirmed by reading `cl_game.c`'s own loader logic first
      rather than guessing. All needed types resolve through headers
      already reachable via the same `engine_includes`/`sdk_includes`
      `use=` libs `ref/soft` depends on (`cdll_int.h`, `ref_params.h`,
      `q_client.h`, `entity_state.h`, `cl_entity.h`, `render_api.h`,
      `cdll_exp.h` last) -- no new build-system plumbing needed, both are
      generic and already proven for two other static-linked modules.
      Build succeeded on the first real attempt (only harmless
      `-Wmissing-prototypes`/implicit-struct-scope warnings, not in this
      project's `-Werror` set); one real signature bug caught before that
      -- `HUD_ConnectionlessPacket` returns `int`, not `void`, per
      `cdll_exp.h`, initially miswritten and fixed before the build.

      Build command extended to
      `--static-linking=filesystem_stdio,ref_soft,client` (section 7
      updated to match, and its `--dedicated=no` was also fixed there --
      that flag doesn't parse with this waf version, `-d`/`--dedicated`
      takes no value). Package size stayed sane (~2.15MB, consistent with
      goals 2/5, not a packaging regression). Not yet run on real hardware
      -- the real test is whether `Host_Error`'s "can't initialize client"
      line is gone and whether goal 5's `SW_UnlockBuffer` diagnostic
      finally fires, closing that goal's open loop too.
- [x] **7. Menu module bring-up (`mainui_cpp`)**: vendor the `3rdparty/mainui`
      submodule (declared in `.gitmodules` but never checked out here, same as
      upstream) at the commit pinned by the sibling pristine repo
      (`510c30c51a9ebabfb703b95872751357a63cd1d5`), plus its own nested
      `miniutl` submodule, following this project's established vendoring
      convention (plain tracked files, no live gitlink -- same as `opus`/
      `opusfile`/`xash-extras`/`bzip2`/`MultiEmulator`). Reordered ahead of
      input/audio/endianness on 2026-07-20 because two Explore agents
      confirmed neither blocks a visible menu: `ref_soft`'s `Draw_Pic`/
      `R_DrawStretchPic` 2D blit path (hardware-validated by goal 5) is the
      only "textured rendering" a menu needs, and `UI_LoadProgs`'s failure
      (`engine/client/dll_int/cl_gameui.c:1335`) is already non-fatal --
      the engine just falls back to `host.allow_console`. The real blocker is
      that no menu module exists to load at all. This folds in the old
      "textured rendering / first menu framebuffer" item, since ref_soft
      already does that part.
      - Top-level `wscript:129` already gates the subproject on
        `x.env.CLIENT and x.env.DEST_OS != 'android'` -- PS3 qualifies
        automatically once the module exists on disk, no top-level wscript
        change needed for registration itself.
        Extend `--static-linking` to include it (mechanism already proven
        for `filesystem_stdio`/`ref_soft`/`client`).
      - `mainui_cpp`'s own `wscript` forces STB-TrueType-only font mode for
        Android/Darwin/NSwitch/PSVita/Emscripten/MAGX when FreeType2 isn't
        available; try real FreeType2 first via ps3dev's prebuilt portlib
        (same `PKG_CONFIG_PATH`/`os.environ` mechanism already used for
        `libvorbis` in goal 2) before adding PS3 to that fallback list.
      - `exports.txt` (`GetMenuAPI`, `GetExtAPI`) is already exactly two
        lines, compatible with the static-linking `objcopy -G` step as-is.
      - Visible-menu confirmation does not require input -- navigation is
        goal 8's job.

      **Build-verified 2026-07-20 (not yet hardware-validated)**: vendored
      `mainui_cpp`/`miniutl` at the pinned commit, full chain (`./waf
      configure --ps3 --static-linking=filesystem_stdio,ref_soft,client,menu
      --disable-mbedtls && ./waf build`) reaches a real `EBOOT.BIN`/
      `EBOOT.pkg` (~2.75MB, sane growth from goal 6's ~2.15MB) inside
      `ps3dev/ps3dev:latest` Docker, confirmed reproducible from a clean
      `rm -rf build` rebuild, not just incremental state. Three real bugs
      found and fixed, none guessed:
      1. **ps3dev's prebuilt `libfreetype.a` needs zlib** (`freetype2.pc`
         has `Libs.private: -lz`, confirmed by reading the file directly),
         but plain `pkg-config --cflags --libs` only emits `Libs.private`
         when asked for a `--static` link -- harmless on the dynamically-
         linked desktop platforms this check was written for (the runtime
         linker resolves the transitive zlib dependency on its own), but
         fatal on PS3 (`undefined reference to inflateInit2_/inflateEnd/
         inflateReset/inflate` from `ftgzip.o` at final link) since the
         whole process is one fully-static ELF with no dynamic linker to
         paper over it. Fixed with a `conf.env.DEST_OS == 'ps3'` branch in
         `3rdparty/mainui/wscript`'s freetype check, requesting
         `--cflags --libs --static` instead of going through the shared
         `check_pkg` helper (whose hardcoded `--cflags --libs` args can't be
         overridden from the call site without a keyword-arg collision).
      2. **`mainui_cpp` bundles its own separate snapshot of `build.h`**
         (`3rdparty/mainui/sdk_includes/public/build.h`) rather than
         including the engine's live copy -- same class of problem already
         found and fixed for `hlsdk-portable`'s own vendored `build.h` (see
         section 6). Its `#elif defined __PPU__` branch was simply missing,
         so every mainui `.cpp` including it hit the `#error` in the
         platform-detection `#else` fallback. Fixed by adding the identical
         `#elif defined __PPU__` -> `#define XASH_PS3 1` branch (plus the
         matching `#undef XASH_PS3` in the undef list at top), mirroring
         `3rdparty/library_suffix/include/build.h`'s existing branch exactly.
         Its separate CPU-detection block (`__PPC__`/`__powerpc64__`) already
         handled PS3 correctly with zero changes needed, same finding as
         `hlsdk-portable`'s.
      3. **The real, systemic one**: `scripts/waifulib/xshlib.py`'s
         `add_deps` (the function that merges each statically-linked
         module's relocatable `.o` into the final `xash` binary) only ever
         added the module's *object file* to `xash`'s source list -- it
         never forwarded that module's own external `use=` library
         dependencies to the final link at all. Latent since goal 2:
         `filesystem_stdio`/`ref_soft`/`client` never needed a fresh
         external library beyond what the engine's own global link already
         carries, so this never surfaced until `menu` became the first
         statically-linked module with a genuinely new external dependency
         (freetype+zlib). Symptom: `menu.o` present on the final `xash`
         link command line, but `-lfreetype`/`-lz` never appended, so
         `undefined reference to FT_Init_FreeType` et al at final link even
         though `mainui_cpp` itself configured and compiled cleanly.
         First fix attempt (forwarding just the uselib *name* `'FT2'` into
         `xash`'s own `use=` list) still failed silently -- root-caused via
         `nm`/cache inspection (not guessed) to a **second, deeper issue**:
         each subproject configures into its own isolated `conf.env` clone
         (confirmed: `build/c4che/3rdparty/mainui_cache.py` has
         `STLIB_FT2 = ['freetype', 'z']`, `build/c4che/engine_cache.py` has
         no `FT2` entry at all), so waf's normal uselib-name resolution
         (`propagate_uselib_vars` looking up `STLIB_FT2`/`LIB_FT2` on the
         *consuming* taskgen's own env) finds nothing -- this is why the
         earlier `ref_soft -> ref_common` STLIB fix (goal 2) never hit this:
         that dependency resolves via real taskgen-to-taskgen linking
         (`get_tgen_by_name`, env-agnostic), not an external pkg-config-style
         uselib string. Final fix: in `add_deps`, for each of a relocatable
         module's own external `use=` names not already known to the main
         binary, copy the already-resolved `STLIB_`/`STLIBPATH_`/`LIB_`/
         `LIBPATH_` *values* from that module's own `tgen.env` directly onto
         `xash`'s env under the same variable names (not just the bare
         name), before appending the name itself to `xash`'s `use=` list --
         letting the standard uselib-name lookup succeed afterward. Generic
         fix in shared tooling, not a `menu`/freetype-specific patch; any
         future statically-linked module with its own new external
         dependency will hit the same path safely.
      **VALIDATED on real hardware 2026-07-20**: `SUCCESS` -- the Half-Life
      main menu rendered and was visible on screen. UDP log confirms a clean
      run all the way through: FS init, asset checksums (same non-fatal
      `gfx/conchars` CRC32 failure as goal 3, still not a blocker), RSX video
      bring-up (`PS3_RSX_Init: ready, 1920x1080`), `mainui_cpp` initializing
      (`UI_ApplyCustomColors`, `Localize_AddToDict`), and finally
      `SW_UnlockBuffer: first call, fb=0 1920x1080` -- the first real
      present of a frame containing the menu. Two new, non-blocking items
      surfaced in the log, neither preventing the menu from displaying:
      - `Unable to read font file gfx/fonts/FiraSans-Regular.ttf!` /
        `tahoma.ttf!` (repeated) -- `MAINUI_USE_FREETYPE` is on (goal 7's
        freetype fix worked build-wise), but the actual `.ttf` files aren't
        present under the shipped `valve`/`extras` asset tree (only license
        files were vendored in `3rdparty/extras/xash-extras/gfx/fonts/`,
        e.g. `FiraSans-OFL.txt`, not the font binaries themselves).
        `mainui_cpp` falls back to its bitmap font path
        (`font/BitmapFont.cpp`, already compiled in) and rendered fine
        regardless -- cosmetic-only gap, revisit only if real TTF rendering
        is wanted later (would need the actual `.ttf` assets FTP'd/vendored,
        not a code fix).
      - `^1Error: ^7can't initialize server:` at boot, before the menu --
        expected and harmless at this stage (no listen-server game logic
        exists yet, goal 6's stub `client` module has no matching server-
        side counterpart); did not prevent the menu from showing.
      - Audio (`PS3 audio backend not implemented yet`) is goal 9's job,
        as already scoped.
- [x] **8. Input**: `ioPad` polling (all 7 ports, DS3 sticks/buttons) mapped
      to `Key_Event` in `in_ps3.c`, verified via on-screen or UDP echo. Also
      wires menu navigation (`pfnKeyEvent`/`pfnCharEvent`) once goal 7 lands.
      **Hardware-validated 2026-07-20: SUCCESS.** `in_ps3.c`
      polls a sticky active port (`MAX_PORT_NUM`, not `io/pad.h`'s
      `MAX_PADS` -- that's a 127-entry virtual/LDD-pad cap, not the
      physical port count; the pre-existing stub had this wrong), maps
      digital buttons to `K_A_BUTTON`/`K_B_BUTTON`/etc (same semantics as
      `joy_sdl2.c`'s `g_button_mapping`) via edge-triggered `Key_Event`,
      and feeds both sticks plus `PRE_L2`/`PRE_R2` trigger pressure into
      the engine's existing generic `Joy_AxisMotionEvent` (deadzone,
      trigger-as-button synthesis, and menu-mode D-pad simulation are
      already handled centrally in `in_joy.c` -- not reimplemented here).
      Found and fixed two real gaps while wiring this up, neither
      PS3-input-specific: `Key_Event` needs `client.h`, not `input.h`
      (`in_dos.c`'s existing PS3-adjacent precedent was already wrong/
      unbuilt, don't copy its include list blindly); and `PS3_InputInit`/
      `PS3_InputShutdown` existed since the goal-7-era stub but were never
      actually called from anywhere -- `IN_Init`/`IN_Shutdown`
      (`engine/client/input/input.c`) now call them under `#if XASH_PS3`,
      mirroring the existing `XASH_USE_EVDEV` call pattern in the same
      function; declarations added to `platform.h` next to Evdev's.
      Stick axis sign convention (which physical direction is positive)
      is unconfirmed -- verify with `joy_debug 1` on hardware and flip
      signs in `PS3_ScaleStick` if backwards.
- [x] **9. Audio**: PSL1GHT audio port (48kHz float32, 256-sample blocks, 8
      blocks) with a notify-queue-paced feeder thread in `s_ps3.c`.
      **VALIDATED on real hardware 2026-07-25**: `SUCCESS` -- audio audible
      through the Half-Life main menu.
      **Build-verified 2026-07-25.**
      Implemented `SNDDMA_Init`/`SNDDMA_Shutdown` plus a new
      `PS3_Audio_ThreadFunc` feeder thread, replacing the "not implemented"
      stub. Confirmed by reading `common/sound_api.h` and
      `engine/client/sound/s_main.c` that Xash's sound backend contract
      differs from the sibling `ioQuake3-PS3` reference
      (`code/audio/ps3_snd.c`): the **engine** owns `snd.buffer` (int16
      interleaved ring, `snd.samples` mono-sample-sized) and mixes into it
      once per frame; the platform backend's only job is to advance
      `snd.samplepos` to reflect real hardware consumption -- there is no
      separate app-owned dma buffer like Q3's. Also confirmed
      `s_mix.c:312` mixes to `snd.format.speed` directly (not a hardcoded
      44100), so the backend honestly reports 48000 Hz and no manual
      resampling was needed. Ported the Q3 reference's init/shutdown
      sequence (`sysModuleLoad(SYSMODULE_AUDIO)` -> `audioInit` ->
      `audioPortOpen`(2ch, `AUDIO_BLOCK_8`) -> `audioGetPortConfig` ->
      `audioCreateNotifyEventQueue` -> `audioSetNotifyEventQueue` ->
      `audioPortStart`) and its thread-priority choice (100, above the
      main thread's 1001) exactly, but skipped its VMX/AltiVec int16->f32
      conversion in favor of a plain scalar loop for this first pass (no
      existing AltiVec precedent in this project; revisit only if a real
      hardware profiling pass shows it matters).

      Found and fixed three real bugs against this toolchain's actual
      headers before the build succeeded, none guessed:
      1. **`sys/thread.h` also trips `-Werror=old-style-definition`**
         (`sysThreadYield()` declared with empty parens, not `(void)`) --
         a different warning class than the `-Wstrict-prototypes` issue
         already known from `net/net.h`/`io/pad.h`. The existing
         `#pragma GCC diagnostic push/ignored "-Wstrict-prototypes"/pop`
         wrap alone wasn't enough; added a second
         `#pragma GCC diagnostic ignored "-Wold-style-definition"` to the
         same wrap.
      2. **`sysThreadCreate`'s `threadname` parameter is non-const
         `char *`**, not `const char *` -- passing a string literal
         directly triggered `-Wdiscarded-qualifiers`. Fixed the same way
         the Q3 reference does it: a `static char[]` buffer instead of a
         literal.
      3. **The "already loaded" sentinel the Q3 reference checks for
         (`0x8001112E`) is not what this toolchain's
         `sysmodule/sysmodule.h` actually defines** -- the real named
         constant is `SYSMODULE_ERR_DUPLICATE` (`0x80012001`). Confirmed
         by reading the header directly rather than trusting the
         reference's magic number; switched to the named constant.
      All `audioPortParam`/`audioPortConfig` field names
      (`numChannels`/`numBlocks`/`attrib`/`level`/`readIndex`/
      `audioDataStart`) and `AUDIO_BLOCK_SAMPLES` (256) were verified
      directly against `$PS3DEV/ppu/include/audio/audio.h` before use, not
      assumed from the reference alone.

      Full chain (`./waf configure --ps3
      --static-linking=filesystem_stdio,ref_soft,client,menu
      --disable-mbedtls && ./waf build`) succeeds inside
      `ps3dev/ps3dev:latest` Docker, producing a real `EBOOT.BIN`/
      `EBOOT.pkg` (~2.76MB, sane growth from goal 7's ~2.75MB).
- [x] **10. Game DLL integration**: vendor `hlsdk-portable` (sibling repo,
      `E:\Users\Matteo\Desktop\HL1\hlsdk-portable`) as the game-logic layer,
      statically linked via the project's existing `--static-linking`/
      `xshlib.py` mechanism, replacing goal 6's stub with the real thing.

      **Reordered ahead of endianness/perf on 2026-07-25**: "First playable"
      (now goal 11) structurally requires real game logic -- goal 6's client
      DLL is a throwaway stub with no HUD/gameplay, so it cannot host a
      loaded, playable map. This is also the only remaining goal the user
      cannot test at all until it lands, since goals 11/12/13 all assume a
      real game DLL exists first.

      **Build-verified 2026-07-25, NOT YET hardware-validated.** Vendored
      hlsdk-portable's `dlls`/`cl_dll`/`game_shared`/`common`/`pm_shared`/
      `public`/`engine`/`external`/`utils/fake_vgui` trees into a new
      `hlsdk-portable/` directory (plain tracked files, same convention as
      `opus`/`bzip2`/`xash-extras` -- kept fully separate from xashPS3's own
      `common`/`pm_shared`/`public` at the repo root, since those are the
      *engine's* copies of same-named-but-different-content headers --
      confirmed by diff, hlsdk-portable's own `common`/`pm_shared`/`public`
      are privately scoped to its own SDK build and never meant to merge
      with an engine checkout's copies).

      **Key discovery: hlsdk-portable already ships its own waf build**
      (`wscript`, `dlls/wscript`, `cl_dll/wscript`), written for the
      Xash3D-FWGS ecosystem with existing PSVita/NSwitch `DEST_OS` branches
      -- adapted (not copied verbatim) into new, simplified wscripts that
      drop hlsdk's own install-path/library-naming/VGUI-toggle machinery
      (all dead weight once statically linked: no `.so` output, no install
      step, VGUI permanently off, GoldSource compat permanently off -- no
      dlopen on PSL1GHT and `input_goldsource.cpp`'s SDL2 dlopen call site
      is unconditional in the file regardless of the `GOLDSOURCE_SUPPORT`
      define, so the whole file is excluded from the client glob rather
      than relying on the define alone).

      **`dlls/exports.txt` needed ~250 entries, not a handful** --
      confirmed by reading the real engine code
      (`engine/server/sv_game.c:1067`, `SV_GetEntityClass` ->
      `COM_GetProcAddress(svgame.hInstance, pszClassName)`, called from
      `SV_AllocPrivateData` for every BSP-spawned entity): the engine
      resolves *every entity classname string individually* by name
      through the same `Lib_Find` table-walk used by every other
      statically-linked module. hlsdk-portable's `LINK_ENTITY_TO_CLASS`
      macro has no self-registering factory table -- pure dlsym-by-name,
      one `extern "C"` symbol per classname. Extracted the full list via
      `grep -rho '^\s*LINK_ENTITY_TO_CLASS(\s*[A-Za-z0-9_]*' dlls
      --include=*.cpp | sed -E 's/^\s*LINK_ENTITY_TO_CLASS\(\s*//' | sort -u`
      (250 unique names), plus `GiveFnptrsToDll`/`GetEntityAPI`/
      `GetEntityAPI2`. Two names initially included turned out to be dead
      code under a normal release build and had to be removed after a real
      link failure surfaced them: `my_monster` (`dlls/tempmonster.cpp`,
      entire file wrapped in `#if 0` -- the SDK's own "how to add a
      monster" template, never meant to compile) and `trip_beam`
      (`dlls/effects.cpp:408`, gated `#if _DEBUG`, correctly absent from a
      release build). `cl_dll/exports.txt` (43 entries, the fixed
      `cldll_func_t` interface set) similarly had one dead entry removed,
      `HUD_ChatInputPosition` (`vgui_SpectatorPanel.cpp`, VGUI-only, correctly
      excluded since VGUI is off).

      **Five real source bugs found and fixed via the Docker build, each
      isolated from actual compiler/linker output, not guessed:**
      1. `common/mathlib.h`'s `IS_NAN` macro did unsafe pointer type-punning
         (`*(int*)&x`), fatal under `-Werror=strict-aliasing`. The engine's
         own `public/xash3d_mathlib.h` already solves this identically with
         plain `#define IS_NAN isnan` -- matched that instead of adding a
         suppression.
      2. Same type-punning pattern (bit-reinterpreting a `float` into `int`
         for a PRNG seed) in `dlls/util.cpp` and the near-identical
         `cl_dll/com_weapons.cpp`, plus byte-buffer marshaling code in
         `cl_dll/demo.cpp`/`hud.cpp` (already flagged by the SDK's own
         `// FIXME: ... *(int *)& crap` comment). Fixed with `memcpy`-based
         type puns (standard-legal, compiles to the same code).
      3. `dlls/zombie.cpp:307`: `(m_Activity == ACT_MELEE_ATTACK1) ||
         (m_Activity == ACT_MELEE_ATTACK1)` -- a genuine copy-paste bug,
         fatal under `-Werror=logical-op`. No second melee-attack activity
         exists in this file, so simplified to the single check rather than
         inventing a second constant.
      4. `cl_dll/entity.cpp` included the legacy `<memory.h>` header, which
         doesn't exist on this libc; replaced with `<string.h>` (declares
         the same `memcpy`/`memset` on every modern libc including this
         one).
      5. **The real cross-module bug, took the longest to isolate**: waf's
         per-taskgen `idx` (used to disambiguate output object filenames,
         `ccroot.py`'s `out = '%s.%d.o' % (node.name, self.idx)`) defaults
         to a counter keyed by the *taskgen's own path*, not by the actual
         directory a source file resolves to. `cl_dll/wscript` compiles
         several weapon `.cpp` files living in `../dlls/` (shared
         client-prediction code) in addition to its own sources; since
         `dlls`'s own taskgen (path `dlls/`) and `cl_dll`'s taskgen (path
         `cl_dll/`) are each the *first* taskgen in their own path, both
         got the default `idx=1`, and the output object path for a shared
         file like `dlls/python.cpp` collided between the two builds --
         waf silently reused one taskgen's compiled object for the other.
         Diagnosed by comparing `nm` output of the real built `.o` against
         a manual standalone recompile with identical flags (which produced
         the missing symbols fine), then finding only one `.o` existed on
         disk where two were expected. Real hlsdk-portable's own
         `dlls/wscript`/`cl_dll/wscript` already carry an `idx =
         bld.get_taskgen_count()` kwarg for exactly this reason (a
         build-wide, not per-path, counter) -- it was dropped as
         apparently-decorative when first adapting the wscripts and had to
         be restored, plus the small `get_taskgen_count()` conf helper
         added to xashPS3's own top-level `wscript` (hlsdk-portable's
         original lived in its own now-unused top-level `wscript`).

      Full chain (`./waf configure --ps3
      --static-linking=filesystem_stdio,ref_soft,menu,server,client
      --disable-mbedtls && ./waf build`) succeeds inside
      `ps3dev/ps3dev:latest` Docker, producing a real `EBOOT.BIN`/
      `EBOOT.pkg`.

      **VALIDATED on real hardware 2026-07-26: the port reaches actual
      in-game play for the first time.** Getting there took a total console
      freeze (power-cycle, no `Sys_Error`) on the first map load, resolved
      over five instrumented hardware rounds.

      **First, the diagnostics had to be rebuilt, because the previous
      session's conclusions were unsound.** Every marker site used its own
      private `static int` counter with its own small budget, so a silent
      marker meant *either* "the code never ran" *or* "this site already
      spent its tickets elsewhere" -- indistinguishable at the listener.
      Concretely, `Host_ServerFrame`'s budget of 3 was incremented at the top
      of the function, before its `if( !svs.initialized ) return;`, and
      `Host_ServerFrame` runs every frame from boot (`host.c`): all three
      tickets were consumed by menu frames that print nothing, making that
      marker dead code by the time gameplay started. The old conclusion
      ("freeze is right after `SV_SendClientMessages` returns") was an
      artifact of this, as was the "UDP packet loss" theory -- the
      `Netchan_TransmitBits` markers had simply been spent on connect
      handshake traffic. Replaced with a single channel, `public/ps3_diag.h`
      + `PS3_Diag()` in `sys_ps3.c`: **one global monotonic sequence number
      on every line** (a gap = real UDP loss; a contiguous run that stops =
      the last line really is the last code executed), one global budget, and
      an explicit enable flag. Every subsequent round had contiguous
      sequence numbers, so no result was ever ambiguous again.

      **Root cause: an ODR violation created by the `--static-linking`
      merge.** `dlls/*.cpp` weapon sources are compiled twice -- into
      `server.o` without `CLIENT_DLL`, and into `client.o` with it (upstream
      relies on these being two separate `.so`s, each resolving
      `PRECACHE_MODEL` etc. through its *own* `g_engfuncs`: the client's
      harmless stubs from `HUD_InitClientWeapons`, the server's real engine
      table). Statically linked into one executable, `objcopy -G` keeps the
      two *function bodies* separate but **only one vtable per weapon class
      survives the final link**. Verified on the real build with `nm`/
      `objdump`, not inferred: two `CGlock::Spawn`/`CGlock::Precache` bodies,
      a single `_ZTV6CGlock`, and that vtable's slots pointing at `0x764228`/
      `0x764270` -- inside the *server* `.opd` region (bracketed by
      `GetEntityAPI2` `0x7608f8` and `GiveFnptrsToDll` `0x764c18`), while the
      client-compiled copies at `0x7765d0`/`0x776600` are dead code nothing
      dispatches to. Same pattern on every weapon class checked
      (`vtables=1, spawn_impls=2`).

      So on the first frame the client reached `ca_active`,
      `HUD_PrepEntity`'s `g_Glock.Spawn()` executed *server*-compiled weapon
      code against the *real* engine table, reaching `pfnPrecacheModel`
      (`sv_game.c`) -> `SV_ModelIndex`/`Mod_ForName` -- a disk model load in
      the middle of client prediction. Hard lock on this no-MMU platform.

      **Fix**: `DEFAULT_CL_LW` in `common/defaults.h` (the established
      per-platform defaults pattern, not a hardcoded branch at the cvar)
      defaults `cl_lw` to `"0"` on PS3, so `HUD_PostRunCmd` takes its `else`
      branch and `HUD_WeaponsPostThink` -- hence `HUD_InitClientWeapons` and
      all client-side weapon `Spawn` calls -- never run. Movement prediction
      (`pfnPlayerMove`) is untouched and already worked. The server stays
      authoritative for weapons, which costs essentially nothing on a
      loopback single-player listen server. **The first attempt at this fix
      silently did nothing**: `cl_lw` is `FCVAR_ARCHIVE`, and an archived
      `config.cfg` from earlier builds set it straight back to 1 after the
      default was registered. Hence `DEFAULT_CL_LW_FLAGS`, which drops
      `FCVAR_ARCHIVE` and adds `FCVAR_READ_ONLY` on PS3 -- this is a platform
      limitation, not a user preference, and `Cvar_CanSet` then rejects the
      stale config outright.

      **The ODR violation itself is still latent** for any other class shared
      between the `dlls/` and `cl_dll/` compiles -- `cl_lw 0` only stops it
      being *exercised* by weapon prediction. A proper fix means keeping each
      statically-linked module's vtables separate (the same "modules must
      stay isolated" invariant `xshlib.py` already enforces for symbols via
      `-d` + `objcopy -G`, extended to COMDAT/vtable data). Revisit if
      another shared-class crash appears.

      Useful comparison found this session: the sibling **Wii/OGC port**
      (`E:\Users\Matteo\Desktop\HL1\Wii\xash3d-fwgs`) statically links the
      same hlsdk into one binary but **never compiles `dlls/*.cpp` into its
      client library** (verified: zero `dlls/` entries in `cl_dll`'s source
      list in `makelibrary.txt`), so it only ever has one copy per weapon
      class and structurally cannot hit this. It also has a
      `29c575e0 ogc: fixed multiple definitions of g_engfuncs` commit
      renaming the *filesystem* module's `g_engfuncs` to `fs_gEngfuncs` --
      a collision this port avoids via `objcopy -G` instead. Worth consulting
      for any future static-link-merge problem. Note it renders at 640x480
      (`DEFAULT_MODE_WIDTH/HEIGHT` for OGC).

      **Left in place deliberately**: the `PS3_DIAG` markers across the
      engine, `ref_soft`, and (as debug scaffolding) `hlsdk-portable`'s
      `hl_weapons.cpp`/`glock.cpp`. The channel is globally disabled -- the
      `PS3_DIAG_ENABLE()` call was removed from `SV_SpawnServer` -- so it
      costs a predictable-branch per site and emits nothing. Re-add that one
      call to arm it for goal 11. Also added: `-fstack-usage` on the
      `ref_soft` build for `DEST_OS == 'ps3'`, which produced the measured
      frame sizes recorded under goal 11 below.
- [x] **11. `ref_gl` on the RSX (`3rdparty/ps3gl`)**: replace software
      rasterization with real RSX rendering, keeping `ref_soft` as the
      `-ref soft` fallback. See section 4 for the architecture.

      **Vertex-ring fix HARDWARE-VALIDATED 2026-07-26**: confirmed the
      32 MB `PS3GL_VRING_SIZE` bump (up from 2 MB) eliminates the
      intermittent dropped-draw glitches -- ref_gl is the confirmed-active
      renderer (not silently falling back to soft), goal fully closed.

      **Inserted ahead of "first playable" on 2026-07-26** (old 11-13 renumbered
      to 12-14) because first-playable should be validated on the renderer that
      actually ships, and both blockers old-goal-11 already documented are
      `ref_soft`-specific: video runs at the TV's native 1920x1080, so `ref_soft`
      software-rasterizes ~2.07M px/frame on an in-order PPE, and
      `R_EdgeDrawing`/`R_DrawBrushModel` peak around 544 KB of stack against a
      1 MB main thread. Moving rasterization to the RSX deletes both.

      **Feasibility was measured, not estimated**, before any code was written:
      `ref_gl`'s 16 renderer `.c` files plus `gl_opengl.c` reference **93
      distinct GL entry points**; the ported ps3gl already implemented **61** of
      them. The 32-symbol gap is closed in `3rdparty/ps3gl/ps3gl_glapi.c`, in
      three groups:
      1. **Extension-gated, never reached** (VBO, compressed textures, 1D/3D
         textures, multisample textures, `glDebugMessage*`): ps3gl's
         `glGetString( GL_EXTENSIONS )` advertises only `GL_ARB_multitexture`,
         so `GL_CheckExtension` fails for all of them and ref_gl takes its
         non-extension path -- but `XASH_GL_STATIC` links the symbols directly,
         so they must still exist. Stubbed.
         `glDrawRangeElements` is the one exception: it forwards to
         `glDrawElements`, which *is* ref_gl's own unextended fallback.
      2. **Trivial completions** backed by existing ps3gl state
         (`glIsEnabled`/`glIsTexture`/`glMultiTexCoord2f`/`glTexEnvfv`/...).
      3. **Fog and texgen: state tracked, not applied.** `gl_rmain.c` reads
         `pglIsEnabled( GL_FOG )` and `pglGetFloatv( GL_FOG_COLOR )` back and
         branches on both, so the state has to round-trip correctly -- it does.
         Actually *rendering* fog and chrome/sky texgen needs new offline
         cgcomp'd program permutations and is deferred to goal 14. Visual gaps,
         not correctness gaps.

      Two things the plan expected to be real work turned out not to be, both
      confirmed by reading the call sites rather than assuming:
      - **`GL_COMBINE_ARB` + `GL_RGB_SCALE 2`** (`gl_rsurf.c:2432`, `:2469`)
        lives entirely in gl_rsurf's VBO path (`R_EnableDetail`,
        `R_SetLightmap`, gated on `gl_overbright`/`r_vbo_overbrightmode`).
        With `GL_ARB_VERTEX_BUFFER_OBJECT_EXT` reported false that code never
        runs, and ps3gl's `glTexEnvf` already folds any unrecognized env mode
        into `PS3GL_TENV_MODULATE`, so nothing had to be written.
      - **`gl2_shim/gl2_shim.c` is entirely wrapped in `#if !XASH_GL_STATIC`**,
        so it compiles to an empty TU here and pulls in no GLES2 symbols.

      Ported from `ioQuake3-PS3/code/gl/` with three deliberate omissions, none
      of them carried over verbatim:
      - `ps3gl_spu.c` (SPU vertex-interleave offload) is excluded -- it needs
        `code/spu/spu_vtx_shared.h` and a separate `spu-gcc` build, and it has a
        scalar fallback. Revisit under goal 14 only if profiling demands it.
      - The `ps3gl_restore_if_needed()` / `.data` backup-pointer / `0x1` sentinel
        machinery in `ps3gl_main.c` is dropped. It is a BSS-corruption band-aid
        specific to that port; if the same symptom shows up here, root-cause it
        instead of inheriting the workaround. `ps3gl_ptr` is now a plain
        `NULL`-initialized pointer and `ps3gl_init` returns success/failure.
      - Its `ps3_log`/`printf` diagnostics now route through `ps3gl_log`, which
        calls the engine's `Con_Printf` (and therefore the UDP sink, via goal
        3's `Sys_PrintStdout` fix). The 30-texture-upload diagnostic dump was
        deleted outright -- ref_gl already logs its own texture loads.

      The `gl*` entry points were renamed from `ps3gl_*` wholesale (the Q3 port
      bound them through a `qgl_ps3.c` function-pointer table; `XASH_GL_STATIC`
      needs the real names). `ps3gl_SetWorldClipPlane` deliberately keeps its
      prefix -- it is a PS3-specific extra, not a GL entry point.
      `3rdparty/ps3gl/include/GL/gl.h`'s enum values were diffed against
      `ref/gl/gl_export.h` before trusting them (no conflicts; only `0` vs `0x0`
      formatting differences), and its `GLintptrARB`/`GLsizeiptrARB` are
      deliberately `int` rather than `ptrdiff_t` to match `gl_export.h`'s
      declarations exactly -- a mismatch there would silently break the ABI
      rather than fail to compile.

      Build wiring: top-level `wscript`'s `DEST_OS == 'ps3'` branch now sets
      `conf.options.GL = True` (it previously forced `False` with the stale
      "no runtime shader compiler" reasoning), a `Subproject('3rdparty/ps3gl')`
      entry gated on PS3, `ref/gl/wscript` forces `XASH_GL_STATIC=1` on PS3
      (deliberately *not* via `--enable-static-gl`, whose configure step runs an
      irrelevant host `check(lib='GL')`), and `engine/wscript` adds `ps3gl` to
      the client link. `ref/gl/gl_opengl.c`'s existing `#if XASH_PSVITA`
      `GL_SetExtension( GL_OPENGL_110, true )` block was extended to PS3 --
      without `VGL_ShimInit`, since ps3gl's own `glBegin`/`glEnd` already batch
      into an RSX vertex ring rather than issuing per-vertex commands.

      Build command gains `ref_gl` (section 7 updated to match):
      `./waf configure --ps3 --static-linking=filesystem_stdio,ref_soft,ref_gl,menu,server,client --disable-mbedtls && ./waf build`

      **Build-verified 2026-07-26, NOT YET hardware-validated.** Full chain
      succeeds inside `ps3dev/ps3dev:latest` Docker, producing
      `EBOOT.BIN` (3,867,136 B) and `EBOOT.pkg` (3,869,264 B) with a correct
      `PARAM.SFO` + `ICON0.PNG` + `USRDIR/EBOOT.BIN` layout. All six
      static-link tables are present in the final ELF
      (`lib_{filesystem_stdio,ref_soft,ref_gl,menu,server,client}_exports`),
      confirming both renderers really are linked, not just configured.
      ps3gl itself is small: 37 KB text / 4 KB data / 197 KB bss, of which
      128 KB is `ps3gl_draw.c`'s static 16-bit index scratch buffer; the
      `ps3gl_state_t` singleton (~920 KB, dominated by the 16384-vertex
      immediate-mode batch and the 4096-slot texture table) is heap, not bss.
      The eight embedded shader blobs total under 1 KB.

      The **linkage boundary was verified with `nm`, not assumed** -- this was
      the one design decision that could have silently produced a
      wrong-but-linkable binary. In `ref_gl.o`, `glEnable`/`glTexImage2D` are
      `U` (undefined), and in the final `xash` ELF they plus `glDrawElements`/
      `glFogfv`/`glIsEnabled`/`ps3gl_init` are `T` (defined, engine side), with
      `nm -u` reporting **no** remaining undefined GL symbols. That is exactly
      the intended split described in section 4.

      Two real build errors were found and fixed, both in ps3gl, neither
      guessed:
      1. `ps3gl_main.c` called `usleep()` in the vertex-ring fence wait without
         including `<unistd.h>` -- fatal under this project's
         `-Werror=implicit-function-declaration`. The Q3 port got away with it
         through a different include chain.
      2. `ps3gl_get_mvp`/`ps3gl_get_mvp_generation`/`ps3gl_inc_draw_count` were
         declared with bare `extern` lines duplicated across three consumer
         `.c` files and never seen by their own definitions
         (`-Wmissing-prototypes`). Moved the declarations into `ps3gl.h` and
         deleted the three sets of duplicates, rather than silencing the
         warning.

      **HARDWARE-VALIDATED 2026-07-26**: ref_gl renders the Half-Life main menu
      and in-game world on real hardware, textured and correct (bus interior and
      Black Mesa exterior confirmed by screenshot). RSX rendering replaces
      software rasterization. Took **three hardware rounds**; the load-bearing
      bug was mine, not ps3gl's.

      **Round 1/2 -- everything rendered untextured (black-and-white menu, flat
      grey studio models).** Geometry, vertex colors, lighting and the flip loop
      all worked; only texturing was dead. Diagnosed by instrumenting
      `ps3gl_shader_key`'s three predicates rather than guessing: the log showed
      `tmu0 enabled=1 bound=<non-NULL> data=0x0`, i.e. textures were bound but
      carried no pixels, so every draw fell through to the color-only fragment
      program. Round 2's probe was placed at the *bottom* of `glTexImage2D`,
      which could not distinguish "never called" from "called but bailed early"
      -- a real diagnostic-design mistake that cost a round. Round 3 moved the
      probes to the entry points and printed a reason for every early return.

      **Root cause (round 3): `vid_ps3.c` never called
      `ref.dllFuncs.GL_InitExtensions()`.** Every other GL-capable backend calls
      it from its own `R_Init_Video` -- `vid_sdl2.c:950`, `vid_sdl1.c:417`,
      `vid_sdl3.c:312` -- but this backend was grown from a `ref_soft`-only one,
      which has no such requirement, so the call was simply absent. That
      function is what sets `glw_state.initialized`, and `GL_UploadTexture`'s
      **very first line** (`gl_image.c:969`) is
      `if( !glw_state.initialized ) return true;` -- it reports **success** and
      uploads nothing. `GL_SetTextureTarget` lives inside that same function, so
      every `gl_texture_t` also kept `target == GL_NONE`; the decisive log
      evidence was `BindTexture ... target=0x0` combined with zero
      `glTexImage2D` calls ever reaching ps3gl. Nothing in ps3gl was wrong: it
      was faithfully drawing textures that had never been given pixels. Fixed by
      calling `GL_SetupAttributes` before the mode set and `GL_InitExtensions`
      after `ps3gl_init`, matching the SDL backends' order. `R_Free_Video`
      already had the paired `GL_ClearExtensions`.

      Two further real bugs fixed in the same pass, both found by reading rather
      than by hardware failure:
      1. **`GL_BGRA`/`GL_BGR` were unhandled.** `gl_image.c:849-853` maps
         rgbdata's `RF_BGRA`/`RF_BGR` straight through, which is the common case
         for GoldSrc WAD/BMP content, but ps3gl came from ioQuake3 whose
         renderer only ever uploads `GL_RGBA`. `gl_format_bpp()` fell to its
         `default: return 4`, so `GL_BGR` would have desynced every row, and
         `GL_BGRA` would have swapped red and blue. Fixed in both
         `convert_pixels` and `glTexSubImage2D` (lightmap updates use the
         latter). Fixed pre-emptively in the same build as the root cause rather
         than costing a fourth round, since the mapping is provably wrong
         independent of any hardware observation.
      2. **Vertex ring was undersized, and failed silently.**
         `ps3gl_vring_alloc` deliberately refuses to wrap mid-frame (wrapping
         would stomp data the GPU has not fetched yet, since submission runs
         ahead of execution) and **drops the draw** instead -- visibly missing
         geometry. The inherited `PS3GL_VRING_SIZE` of 2 MB gives 1 MB per
         segment = ~29k verts at 36 B each, which Half-Life exceeds at 1080p
         (hardware log: `need 35676, have 25168 of 1048576`). Worse, the
         warning was `static int warned` -- printed **once per process**, hiding
         how often it really happened. Raised to 32 MB (16 MB/segment,
         ~466k verts/frame, against 256 MB of GDDR3 of which the 1080p
         framebuffers and depth take ~25 MB), and replaced the once-only
         warning with a per-frame dropped-draw tally plus a `peak_head`
         high-water mark reported on 256 KB steps, so the ring can be sized from
         a real play session instead of guesswork. **Build-verified, not yet
         hardware-validated** -- the user reported intermittent visual glitches
         consistent with dropped draws.

      All round-1/2/3 `[ps3gl/diag]` instrumentation was removed once the cause
      was identified; only the permanent vertex-ring telemetry remains.

      **Still open, expected, and scoped:** fog and chrome/sky texgen render as
      no-ops (see item 3 of the API-gap list above) -- they need new
      offline-cgcomp'd program permutations, deferred to goal 14.
- [x] **12. First playable**: full HUD, input, audio, a loaded map.
      **HARDWARE-VALIDATED 2026-07-26**: campaign fully playable across
      multiple levels -- HUD renders correctly, audio audible throughout
      (not just menu), input responsive, no crashes on level transitions.

      **Known measurement going in** (from `-fstack-usage`, this build, not
      estimated): `R_EdgeDrawing` 400448 B and `R_DrawBrushModel` 400384 B
      each declare their edge/surface arrays as plain locals, and each calls
      `R_ScanEdges` (144288 B) *inside* that frame (`r_main.c:966`, `:1024`)
      -- a nested peak around **544 KB against a 1 MB main thread stack**
      (`SYS_PROCESS_PARAM( 1001, 0x100000 )`), before counting the engine
      call chain above it or `R_RenderWorld`'s recursive BSP descent. The
      cvar defaults confirm those arrays really are stack-resident rather
      than heap (`r_main.c:1243-1276`): `sw_maxsurfs` "0" -> `MINSURFACES`
      2000 -> `r_surfsonstack = true`; `sw_maxedges` "32" -> `MINEDGES` 4000
      <= `NUMSTACKEDGES` -> `auxedges = NULL`. A runtime watermark is already
      wired (`PS3_DIAG_STACK_TOP()` in `Host_Frame`, `PS3_DIAG_STACK()` at
      `R_EdgeDrawing`/`R_DrawBrushModel`, printing only on a new maximum) and
      will report the real depth as soon as the diagnostic channel is armed.
      Note also that video currently runs at the TV's native 1920x1080, so
      `ref_soft` is software-rasterizing ~2.07M pixels/frame on an in-order
      PPE; the Wii port targets 640x480 for comparison. **Both of these are
      what motivated inserting goal 11 (`ref_gl` on the RSX) ahead of this
      item** -- they are `ref_soft`-specific and go away once rasterization
      moves to the GPU. They were never goal-10 regressions. The stack
      watermark stays relevant only for the `-ref soft` fallback path.
- [x] **13. Endianness audit**: confirm `XASH_BIG_ENDIAN` is set correctly by
      the toolchain and audit any raw-struct reads (WAD, save files, network)
      not already covered by `public/swaplib.h` (MDL/BSP/SPR are covered).
      Deferred behind 10/12 -- real game-DLL code (save-game serialization,
      AI node graphs, network messages) is the highest-value endianness
      surface, so auditing it before that code exists would be premature;
      `hlsdk-portable`'s own `common/byteswap.h` swap macros are already
      confirmed real and working (see section 6).

      **Started 2026-07-27.** An Explore pass (read-only) found six real,
      unswapped raw-struct/scalar sites not already covered by
      `public/swaplib.h` or `hlsdk-portable/common/byteswap.h`'s existing
      CSave/CRestore and CGraph::Byteswap* paths -- confirmed already-safe:
      `net_encode.c` (in-memory only, never raw wire bytes), the `*(int*)==-1`
      out-of-band checks (`-1` is byte-order-invariant), WAV/WAD/BMP loaders
      (raw read but already wrapped in `Little*`/`le_struct_swap` right after).
      All six were fixed and **build-verified** (full
      `./waf configure --ps3 --static-linking=filesystem_stdio,ref_soft,ref_gl,menu,server,client --disable-mbedtls && ./waf build`
      inside `ps3dev/ps3dev:latest` Docker, clean compile + link + EBOOT.pkg),
      **NOT YET hardware-validated** (nothing here is visually/interactively
      observable without a second machine or a cross-endian demo/save file --
      see the per-site notes below for what a hardware pass should check):
      1. **`engine/common/net_ws.c`** -- `SPLITPACKET`/`SPLITPACKETGS` header
         (`net_id`/`sequence_number`/`packet_id`) was cast raw onto the UDP
         datagram buffer on both send and receive, with zero swap calls in
         the file. `NET_HEADER_SPLITPACKET` is `-2`, NOT byte-order-invariant
         like the `-1` out-of-band header (0xFFFFFFFE swaps to 0xFEFFFFFF),
         so split-packet detection would silently fail against real x86
         GoldSrc/Xash peers. Fixed with `LittleLong`/`LittleShort` at every
         read/write site (`net_buffer.c`'s existing bit-writer already uses
         this exact idiom for the rest of the wire format).
      2. **`engine/client/parse/cl_parse.c:1616`** (`CL_SendConsistencyInfo`,
         GoldSrc-protocol only) -- patched a length placeholder with a raw
         `*(short *)&msg->pData[pos] = len` instead of routing back through
         `MSG_WriteShort`. Fixed with `LittleShort`.
      3. **`engine/common/hpak.c`** (`.hpk` custom resource pack, explicitly
         a cross-machine format per its own header comment) -- zero swap
         calls anywhere. Added `dresource_swap`/`hpak_header_swap`/
         `hpak_lump_swap` `swaplib.h` descriptors (same convention as
         `img_wad.c`'s WAD/mip tables) and a `HPAK_SwapLumps()` helper, wired
         into all ~10 read/write call sites across `HPAK_CreatePak`,
         `HPAK_AddLump`, `HPAK_Validate`, `HPAK_ResourceForHash`,
         `HPAK_ResourceForIndex`, `HPAK_GetDataPointer`, `HPAK_RemoveLump`,
         `HPAK_List_f`, `HPAK_Extract_f`. `HPAK_RemoveLump`'s header
         passthrough copy needed care: write the raw wire-order bytes
         through untouched FIRST, only then swap the in-memory copy to host
         form for the function's own use -- swapping before that copy would
         have corrupted it.
      4. **`engine/client/cl_demo.c`** (`.dem` demo format, shared across
         machines in the HL community) -- zero swap calls in the whole file.
         Added `demoheader_swap`/`demoentry_swap` descriptors plus per-site
         `LittleLong`/`LittleShort`/`LittleFloat` wraps for the many scalar
         sequence/length/angle fields recorded and played back
         (`CL_WriteDemoSequence`/`CL_ReadDemoSequence`,
         `CL_WriteDemoUserCmd`/`CL_ReadDemoUserCmd`, message-length prefixes,
         Quake1-demo viewangles). `demo.header` and `hash_pack_header`-style
         persistent structs are never swapped in place -- always via a
         scratch copy -- since their fields (e.g. `demo.header.host_fps`)
         are read in host form continuously elsewhere in the same recording/
         playback session.
         **Bonus, non-endianness bug found and fixed in the same code path**:
         `CL_DemoReadMessage`'s `dem_userdata` handler did
         `FS_Read( cls.demofile, &size, sizeof( int ))` directly into a
         `size_t size` local -- 8 bytes on this LP64 target. On big-endian
         that lands the 4 wire bytes in the HIGH 32 bits of the variable
         instead of the low ones (correct only by accident on little-endian
         hosts), producing a huge garbage allocation size regardless of the
         byte-swap fix. Fixed by reading into a proper `int` temporary first.
      5. **`engine/server/sv_save.c`** (save-game container) -- the bulk of
         the actual save data (`GAME_HEADER`/`SAVE_HEADER`/`SAVE_CLIENT`/
         entity table/decals/statics/sounds/lightstyles/adjacency/temp
         entvars) already round-trips safely through
         `svgame.dllFuncs.pfnSave{Read,Write}Fields`, i.e. the game DLL's own
         already-confirmed-safe `CSave`/`CRestore` (`dlls/util.cpp`) -- not
         touched. What sv_save.c itself reads/writes raw and DID need
         fixing: every container header's `id`/`version`/`size`/
         `tokenCount`/`tokenSize`/`tableCount` int
         (`GetClientDataSize`, `LoadSaveData`, `SaveClientState`/
         `LoadClientState`, `SaveGameState`, `SaveGameSlot`/
         `SaveReadHeader`), `DirectoryCopy`/`DirectoryExtract`'s per-file
         `fileSize` (the `.HL1`-`.HL3` companion files), `EntityPatchWrite`/
         `EntityPatchRead`'s patch count and entity indices (`.HL3`), and
         **`SV_GetSaveComment`'s independent, unsynced re-parse of the
         CSave-produced token stream** -- its `*(short *)pData` field-size
         and token-index reads bypassed `CRestore::ReadShort` entirely and
         needed `LittleShort` wraps at both occurrences (the initial
         `GameHeader` field and the per-field loop). All fixed with
         `LittleLong`/`LittleShort`, always via a local `wire` temporary
         when the source was a persistent `pSaveData` field still needed in
         host form afterward (never swapped in place).
      6. **`hlsdk-portable/game_shared/voice_banmgr.cpp`** -- `CVoiceBanMgr::
         Init`/`SaveState`'s single `int version` field read/written via raw
         `fread`/`fwrite`. Fixed with `LittleToHostSW` (this file's own
         `common/byteswap.h`, not the engine's `xash3d_types.h` -- it's a
         `game_shared` file built into the client/server game DLL). Lowest
         stakes of the six (local per-user file), included since the user
         asked to fix all six findings in one pass.

      Full chain (`./waf configure --ps3
      --static-linking=filesystem_stdio,ref_soft,ref_gl,menu,server,client
      --disable-mbedtls && ./waf build`) succeeds inside
      `ps3dev/ps3dev:latest` Docker -- clean compile of all five touched
      engine files plus `hlsdk-portable/game_shared`, link, strip, sprxlink,
      self, and `EBOOT.pkg` packaging (`ContentID
      UP0001-XASH10000_00-0000000000000000`, package size ~3.87 MB, in line
      with prior goals). **Hardware pass still needed**: none of these six
      bugs are observable on a single console alone (split-packet detection
      needs a real client/server pair or packet capture; the `.dem`/`.hpk`/
      `.sav` fixes need either a demo/save file authored on a little-endian
      x86 build loaded on this PS3 build, or vice versa, to actually exercise
      the swap path -- a same-PS3 round trip through these fixed functions
      will pass even without them, since writer and reader now agree with
      each other regardless of which direction is "correct"). Regular
      solo play (record/load a demo or savegame on this same PS3 build) is
      NOT a valid test for this goal -- it would have "worked" even before
      the fix. Needs either a cross-endian save/demo file or is otherwise
      accepted on code-review confidence alone.

      **HARDWARE-VALIDATED 2026-07-27, partially.** Real, meaningful
      confirmations, not same-console-only round trips:
      - **Crossplay against a real remote GoldSrc server** (`193.111.77.184:
        27011`, `BUILD 4109`, Sentinel-anticheat-protected public DM server)
        -- full connect, signon, resource download, sky load, client
        connected at 6.99s, no split-packet corruption. This is the real test
        the `net_ws.c` fix needed: a genuine little-endian x86 peer, not a
        same-console loopback. Confirms both the split-packet endianness fix
        and the `PROTOCOL_VERSION`/`PROTOCOL_GOLDSRC_VERSION` match against a
        live server. One unrelated non-fatal log line surfaced:
        `CL_ParseUserMessage: No pfn ReqState 61` -- an unhandled usermessage
        from that server (likely an AMX/plugin-added message), not a core HL
        message, not a crash, not connected to anything touched this session.
      - **Save/load regression confirmed working** on real hardware (user:
        "saving and loading works") -- exercises `sv_save.c`'s fixed
        container-header swaps end-to-end on this build. Same caveat as
        before still applies in the strict sense (same-console save+load
        alone can't distinguish "correctly swapped" from "not swapped at
        all"), but combined with the code-level fix and the live cross-peer
        network confirmation above, this is accepted as sufficient without a
        literal cross-endian `.sav` file.
      - **Demo record/playback**: not explicitly retested this round --
        revisit if it comes up, low risk given the same swap-descriptor
        pattern already proven correct in `hpak.c`/`sv_save.c`.
      - **hpak.c** / **voice_banmgr.cpp**: not exercised (no custom content
        packs or voice-ban list touched this session) -- lowest-traffic of
        the six, accepted on code-review confidence.

      **Unrelated bug surfaced during this hardware round, NOT part of goal
      13, not caused by anything touched this session**: creating a listen
      server crashes (clean `Host_Shutdown`, not a freeze) with
      `_Mem_Alloc: out of memory (alloc size 170 Mb at
      ../engine/server/sv_init.c:871)`. Root-caused and fixed under goal 14
      on 2026-08-03 -- see that entry. (The hypothesis originally recorded
      here, "`SV_UPDATE_BACKUP`/`NUM_PACKET_ENTITIES` not scaled down for
      `svs.maxclients == 1`", was **wrong**: `sv_init.c` already does exactly
      that scaling, and the failure was a 32-player listen server, not a
      singleplayer one.)
- [ ] **14. Performance pass** (real hardware only) + release
      hardening (allocation-failure paths, controller hot-plug, long-session
      soak for command-buffer wrap / memory creep). Includes right-sizing
      `PS3GL_VRING_SIZE` (currently 32 MB, raised from 2 MB in goal 11 to kill
      dropped-draw glitches -- confirmed sufficient, not confirmed optimal;
      no real peak-usage measurement exists yet beyond "stays under 32 MB",
      so don't guess a smaller number without profiling data first).

      **Started 2026-07-27.**

      **`clip_dist` buffer overflow -- FIXED, build-verified.**
      `3rdparty/ps3gl/ps3gl_draw.c`'s clip-plane path wrote
      `clip_dist[PS3GL_MAX_VERTS]` (16384 entries) over an unbounded
      `num_verts` range and read it back using raw uint16 index values (up
      to 65535) straight from the index buffer -- a genuine BSS overflow
      into the adjacent 128 KB `idx16` scratch buffer if triggered.
      Currently dormant only because xash's world rendering is
      immediate-mode and never calls `glDrawElements` (the indexed path
      this bug lives in) -- confirmed by reading the actual call sites, not
      assumed safe. Fixed by clamping the write loop to `PS3GL_MAX_VERTS`
      and skipping any triangle whose index falls outside what was actually
      populated, rather than trusting `num_verts` to stay in range. Ten-line
      fix, no behavior change on any real playthrough today (still dead
      code), done first since it was cheap and certain -- second opinion
      from Opus flagged this as the correct place to start release
      hardening: a dormant memory-safety bug on a platform with no MMU
      fault to catch it is worse debt than it looks, and the fix cost
      nothing to get right.

      **Frame-time telemetry -- added, build-verified.**
      `3rdparty/ps3gl/ps3gl_main.c`'s `ps3gl_end_frame()` now aggregates
      avg/min/max frame time over a 5-second window (via
      `Platform_DoubleTime()`, hand-declared the same way `Con_Printf`
      already is in this file, to avoid pulling `platform.h`'s header chain
      into a 3rdparty TU) and reports once per window through `ps3gl_log`.
      Deliberately NOT routed through the `PS3_DIAG` channel (`ps3_diag.h`)
      -- that channel is a budgeted, one-shot freeze-hunting tool by
      design, the wrong shape for continuous per-frame telemetry; per-frame
      logging would also itself perturb the timing being measured. This
      mirrors the existing vertex-ring `peak_head` reporting pattern in the
      same function exactly (aggregate, then log on a threshold/interval,
      never per-frame).

      **Real bug found via this telemetry: hard 30 fps quantization, root-
      caused and FIXED, hardware-validated 2026-07-27.** A hardware
      playtest with the new telemetry showed steady gameplay locked to a
      tight band around `avg 33.3ms` (`min` rarely below ~31ms), while
      menu/loading frames in the same log showed `min` as low as 4-18ms --
      the signature of vsync quantization, not organic GPU/CPU load (real
      load-bound framerate wanders more than a rigid ~2ms window).

      Root cause, confirmed by reading `engine/platform/ps3/vid_ps3.c`:
      `PS3_FB_COUNT` was `2`, and `PS3_RSX_WaitForFreeBuffer()`'s block
      condition (`(flip_queued - flip_completed) > PS3_FB_COUNT - 2`)
      reduces to `> 0` at that count -- the CPU must block until the
      *previous* flip has been fully scanned out before starting the next
      frame's draw. Combined with `GCM_FLIP_VSYNC`, any frame whose GPU
      work exceeds one 16.6 ms vsync interval gets bumped a full extra
      vsync tick, quantizing effective framerate to a hard 30 fps
      regardless of real GPU cost.

      **This was a transcription bug, not a design choice** -- confirmed by
      reading the sibling `ioQuake3-PS3` port
      (`E:\Users\Matteo\Desktop\quake3\Ioquake3-PS3\ioQuake3-PS3\code\sys\
      ps3_glimp.c`), which `vid_ps3.c`'s own comments already claimed to
      mirror for this exact wait formula. The sibling uses `RSX_FB_COUNT 3`
      with its own comment explaining why: "Block only if both
      non-displayed buffers are in flight... With 3 buffers one buffer is
      always free for the CPU to render into." xashPS3 had copied the wait
      formula but left `PS3_FB_COUNT` at 2, leaving exactly one buffer of
      slack out. Consulted Opus for a hardware-safety second opinion before
      touching this (reviewed both files directly, not just this
      description): confirmed 3 registered display buffers is well within
      `gcm_sys.h`'s documented `bufferId` range (0-7), confirmed no GDDR3
      budget concern (3x color + 1 depth buffer at worst-case 1080p is
      ~32 MB of 256 MB GDDR3), confirmed the wait formula generalizes
      correctly at the new count with no desync risk between
      `ps3_current_fb` and the flip-handler counters, and flagged one real
      gap while reviewing: the timeout path in `WaitForFreeBuffer` didn't
      resync `ps3_flip_completed` to `ps3_flip_queued` on a stall (unlike
      the sibling's `PS3_RSX_WaitFlips`, which does exactly that before
      breaking) -- left unfixed, a single timeout would leave a permanent
      skew that makes the wait fire early forever afterward. Fixed both in
      the same pass: `PS3_FB_COUNT` raised to 3, and the timeout path now
      force-resyncs and logs a warning, matching the sibling exactly. No
      other code changes needed -- `ps3_color_offset`/`ps3_color_buffer`
      arrays and `PS3_RSX_SetRenderTarget`'s indexing were already generic
      over `PS3_FB_COUNT`.

      **Hardware-validated 2026-07-27**: steady gameplay frame time moved
      from a rigid `avg 33.3ms / min ~31ms` band to `avg 17.3-17.5ms
      (~57 fps)` with `min`/`max` spreading into a wide, organic band (e.g.
      min 7.41ms, max 43.72ms) -- confirms the vsync-doubling quantization
      is gone, not just a different quantization level. `gcmSetDisplayBuffer
      [2] ret=0` confirmed clean in the init log (third buffer registered
      correctly), and no "flip fence timed out" warning appeared during
      normal play. Effectively a 2x real framerate win from a two-line fix
      once correctly diagnosed. Input latency rises by up to one extra
      frame as an accepted tradeoff (per Opus's review) -- not separately
      measured, revisit only if it's ever reported as noticeable.

      **Follow-up investigation: is locked 60fps achievable? Yes, at 720p.**
      Added draw-call-flush and vring-fence-wait telemetry to the same
      `ps3gl_end_frame` window (`3rdparty/ps3gl/ps3gl_vertices.c`'s
      `flush_immediate()` now increments a real per-frame flush counter --
      the old `ps3gl_frame_draw_count` only ever incremented from
      `glDrawElements`, which xash never calls, so it always read zero for
      real work). Hardware data ruled out the CPU-side fence wait as a
      factor (consistently 0.01-0.02ms, ~0% of frame time, every window) and
      also ruled out flush-count as directly causal (819 flushes measured
      faster than 288 flushes in different windows -- an inverse
      relationship a real per-flush-cost bottleneck could not produce).

      A clean 1080p-vs-720p hardware comparison (same campaign content, same
      draw-call range) settled it: at 1080p, busy scenes regularly missed
      the 16.6ms vsync deadline (avg 17.5-22ms, dropping to ~50-58fps). At
      720p, the exact same content held a genuinely locked ~60fps almost the
      entire session (many consecutive 300-frame/5-second windows at `avg
      ~16.7ms, min 13-15ms`, regardless of draw-flush count ranging 200-1500
      across those windows). Confirms the renderer is GPU fill-rate-bound at
      1080p, not CPU/draw-call-bound -- the honest, expected shape for a
      launch-era immediate-mode GL1.1-over-RSX renderer at 1080p.

      **Shipped fix: hardcoded 720p output, hardware-validated.** Consulted
      Opus on whether render resolution and output-signal resolution could
      be decoupled (render 720p internally, output a real 1080p signal via
      GPU upscale) -- confirmed via PSL1GHT's real `sysutil/video.h`
      (read directly inside the `ps3dev/ps3dev:latest` Docker image) that
      `videoConfigure` genuinely renegotiates the physical HDMI/AV signal;
      there is no PS3-side "render internally at X, output Y" mechanism
      exposed to homebrew. A true decoupled upscale would need a real
      offscreen RSX render target plus a fullscreen-quad blit pass -- new
      render-target/texture code with real black-screen risk (the same
      class of silent-failure surface-config bug goal 4/11 already hit
      twice) -- rejected as not worth it for a hobby project.

      Instead, `PS3_RSX_Init` (`engine/platform/ps3/vid_ps3.c`) now requests
      `VIDEO_RESOLUTION_720` unconditionally (falling back to the TV's
      actual reported mode only if `videoGetResolutionAvailability` says
      720p genuinely isn't offered), rather than slaving render resolution
      to whatever `videoGetState` reports as currently connected. This
      mirrors the sibling `ioQuake3-PS3` port's `PS3_RSX_Init`
      (`code/sys/ps3_glimp.c`) exactly -- confirmed real and
      hardware-validated there first by reading the actual file, not
      assumed. The TV receives a real 720p signal and does its own
      upscale to whatever it natively displays; this is a real
      sharpness-for-framerate tradeoff (same choice many real launch/
      cross-gen PS3 titles made), not a free win, and requires no PS3
      system-settings change from the user -- the console renegotiates its
      own HDMI output on boot. Build-verified; not yet re-confirmed on
      hardware as a permanent default (the *manual* 720p test that produced
      the locked-60 telemetry above was done via the console's own XMB
      display settings, before this code-level change existed).

      **New, unrelated bug surfaced in the same hardware round, NOT caused
      by anything touched this session**: `RestoreDecal: couldn't restore
      entity index N` fires repeatedly (~30+ times, indices seen: 24, 29,
      37) during a save/load sequence through map transitions
      c1a1 -> c1a1a -> c1a1f -> c1a1. Not present in earlier session logs.
      Not yet investigated -- possibly adjacent to (but distinct from) the
      FIELD_FUNCTION save/restore fix from the c0a0e softlock
      investigation, since both are entity-index/save-restore issues, but
      this is a different symptom (decal restore, not a freeze) and has not
      been root-caused. Tracked here for a future pass; not blocking.

      **Listen-server 170 MB OOM -- FIXED and HARDWARE-VALIDATED 2026-08-03.**
      Listen server starts, map loads, gameplay reached, a second client
      connected, and **crossplay with PC clients still works** -- which is the
      specific outcome the chosen fix was designed to protect (see the two
      rejected alternatives below, both of which would have touched that path).
      Folded in from goal 13's hardware round.
      `_Mem_Alloc: out of memory (alloc size 170 Mb at
      ../engine/server/sv_init.c:871)`, clean `Host_Shutdown`, not a freeze.
      (Line 871 is stale -- the allocation is at `sv_init.c:905` in current
      HEAD; that log came from a `c66973a`-era build.)

      **The hypothesis previously recorded here was wrong, and is kept only so
      nobody re-derives it.** It claimed `SV_UPDATE_BACKUP`/
      `NUM_PACKET_ENTITIES` weren't "scaling down for `svs.maxclients == 1`".
      They are: `sv_init.c:900` already reads `SV_UPDATE_BACKUP =
      ( svs.maxclients == 1 ) ? SINGLEPLAYER_BACKUP : MULTIPLAYER_BACKUP;`.

      The arithmetic identifies the real case exactly. `svs.num_client_entities
      = svs.maxclients * SV_UPDATE_BACKUP * NUM_PACKET_ENTITIES`, times
      `sizeof( entity_state_t )` = 340:

      | maxclients | backup | packet ents | total |
      |---|---|---|---|
      | 1 (singleplayer) | 16 | 256 | 1.33 MB |
      | **32 (listen server)** | **64** | **256** | **178,257,920 B = exactly 170.0 MiB** |

      The reported size matches the 32-player row byte-for-byte, so this was a
      **32-player listen server, never a singleplayer one**. Upstream says the
      same thing in its own comment at `netchan.h:79`: `#define
      NUM_PACKET_ENTITIES 256 // 170 Mb for multiplayer with 32 players`. Not a
      PS3 bug at all -- stock 32-player sizing meeting a ~190 MB console, as one
      contiguous `Z_Realloc` run, with ~105 MB already resident after a
      singleplayer session. `3rdparty/mainui/menus/CreateGame.cpp:119` lets the
      player pick anything from 2 to `MAX_CLIENTS`, so 32 is one menu away.

      **Fix**: cap the listen server at 4 players on PS3 (4 x 64 x 256 x 340 =
      22.3 MB). New `DEFAULT_MAX_LISTEN_CLIENTS` in `common/defaults.h` --
      `4` in the existing `XASH_PS3` block, `MAX_CLIENTS` in the `#ifndef`
      fallback section, so every other platform is bit-identical to before and
      **no `#if XASH_PS3` enters `engine/server/` at all**. `SV_SetupClients`
      bounds the listen-server branch by it and logs when the cap actually bit.
      The dedicated branch is deliberately left alone -- its lower bound is 4,
      and a platform cap below that would invert `bound()`'s range for no gain
      on a target that builds no dedicated server. Singleplayer is untouched
      (`bound( 1, 1, 4 )`).

      **Known cosmetic gap, confirmed on hardware and deliberately not fixed
      yet**: the Create Game menu still *displays* `maxplayers 32` (rendered as
      "32," -- the trailing comma is part of the same artifact). The engine
      clamp is authoritative regardless, which is why the server starts anyway:
      the menu sends `maxplayers 32`, `SV_SetupClients` clamps to 4, 22.3 MB is
      allocated, done.

      The planning assumption that the menu would self-correct via
      `sv_init.c:898`'s existing `Cvar_FullSet( "maxplayers", ... )` was
      **wrong**, and the reason is a reusable mainui trap worth knowing:
      `CMenuField`s in `CreateGame.cpp` are `LinkCvar`'d but their `onCvarGet`
      handlers call `UI_GetScriptCvar()`, and that function
      (`3rdparty/mainui/menus/dynamic/ScriptMenu.cpp:531`) searches the
      `settings.scr` variable list *first* and only falls back to
      `EngFuncs::GetCvarString()` when no such list is loaded. Half-Life's
      `valve/settings.scr` defines `maxplayers`, so the field reads that
      shipped default, never the engine cvar. `SaveCvars()`
      (`CreateGame.cpp:334`) then writes the menu buffer back into the same
      script list. **A menu field being `LinkCvar`'d does not mean it reflects
      the engine cvar.**

      When fixing the display: do NOT put the cap constant in mainui -- it
      carries its own **vendored** `sdk_includes/` snapshot of
      `xash3d_types.h`, which is the duplicated-`build.h` trap from goals 7 and
      10. The clean route is a read-only engine cvar carrying the cap (same
      shape as `host_lowmemorymode`, `host.c:1278`) that `CreateGame.cpp`
      clamps against through the existing `EngFuncs::GetCvarFloat` -- no header
      duplication, and it also repairs the `CreateGame.cpp:119` input
      validation, which currently still accepts anything up to `MAX_CLIENTS`.

      **Two alternatives considered and rejected, with reasons:**
      1. `XASH_LOW_MEMORY=1` -- a blunt flag with ~20 unrelated effects, several
         harmful here: it forces `mainui` off FreeType onto stb_truetype
         (`3rdparty/mainui/wscript:67`, adjacent to the font trap that already
         cost a session), truncates studio texture data (`mod_studio.c:1135`),
         and changes `MAX_MODELS`/`MAX_RESOURCES`/`MAX_VISIBLE_PACKET` in
         `protocol.h`, which *are* wire-visible on the PC-crossplay client path.
         Its renderer savings would be zero anyway -- every `ref/` site it
         touches is in `ref/soft/`, not the default `ref_gl`.
      2. Shrinking `NUM_PACKET_ENTITIES` on PS3 -- verified protocol-safe (only
         3 uses, none on the wire; the wire entity count uses
         `MAX_VISIBLE_PACKET_BITS`), but it also shrinks the **client** ring at
         `cl_game.c:1001`, degrading delta history when joining real GoldSrc
         servers -- the exact path goal 13 hardware-validated. Don't touch the
         validated path to fix the unvalidated one.

      Worth reading on the validation run: `PS3_ProbeMemory` already fires at
      `sv_init.c:1026` on every `SV_SpawnServer`, so the log carries the largest
      contiguous block at listen-server startup -- the first real measurement of
      that number, and the only sound basis for ever raising the cap.

      **Perf round "goal 17" -- studio `fill`, HARDWARE-VALIDATED 2026-08-02.**
      Continues the entity/studio work of the previous rounds (host ring,
      `r_studio_fastcolor`/`r_studio_hostarrays`), which had left the ranked
      remainder `fill` 2.89 / `light` 1.98 / `bones` 1.88 with the heaviest
      measured scene ~0.5ms short of a locked 60fps. This round took `fill`.

      All changes in `ref/gl/gl_studio.c` (plus the cvar decl/registration in
      `gl_local.h`/`gl_opengl.c`):
      - The three `R_StudioBuildArray*` loops now keep `numverts`/`numelems`
        in **locals**, written back once per mesh. They live in `g_studio`,
        which the vertex stores also point into (`arrayvert_bss` is a member,
        and the compiler cannot prove the host ring isn't), so every increment
        was forcing a reload -- a store/load round trip per emitted vertex.
        `R_StudioBuildIndices` became a `static inline` taking those locals.
      - Vertex color is now written as **one aligned 32-bit store**:
        `lightbytes[][3]` became `lightcolors[]` (`uint32_t`, big-endian
        R8G8B8A8 with A=0), and `studio_arrayvert_t.color` became a
        `union { GLubyte bytes[4]; uint32_t packed; }` -- a union rather than a
        pointer cast, because this project builds `-Werror=strict-aliasing`.
      - The alpha byte (`tr.blend * 255`) is converted **once per mesh**, not
        once per emitted vertex. Exact, since `tr.blend` only changes per mesh.
      - New `r_studio_fastfill` (default 1) + `R_StudioBuildArrayNormalMeshFast`:
        the common mesh path with texcoords from a 2048-entry short->float
        table and no per-vertex cvar re-test. Selected once per submodel, so
        no per-vertex branch was added to pay for it.

      **Hardware result** (c4a2, same ~46-50 models vantage as the previous
      round), 15+ consecutive 5s windows: `fill` **2.82-2.89 -> 0.56**,
      `build` 3.96 -> 1.46, `studio` 8.13 -> 5.54, `entities` 10.31 -> 7.46,
      frame **17.21ms (58.1fps) -> 16.68ms (60.0fps), locked**. `flip wait`
      went 1.1ms (7% of frame) -> 3.4ms (20%): at this vantage the PPE is no
      longer what the frame waits on, the GPU/vsync is.

      **The in-session A/B corrected the stated thesis and is worth keeping.**
      The round was built on the prediction that per-vertex int->float
      texcoord conversion dominated, because the PPE has no GPR<->FPR path and
      every such conversion is a load-hit-store. `r_studio_fastfill 0` vs `1`
      at the same spot measured `fill` **0.65 vs 0.56** -- the table is worth
      only ~0.09ms, i.e. ~14% of what remains, not the bulk. The real 2.26ms
      came from the three unconditional changes above (local counters, packed
      color store, hoisted alpha), which are not separable from each other
      without another build and were judged not worth a hardware round to
      attribute further. Kept `r_studio_fastfill` default 1 -- it is a real
      measured win, just not the one predicted.

      Remaining ranked studio cost at this vantage: `client` 2.89 (of which
      `bones` 1.80), `light` 1.78, `fill` 0.56, `submit` 0.33. None of them
      buy anything here while the frame is GPU-bound at 60fps; they only
      matter for scenes heavier than c4a2.

      **Boot black-screen time -- HALVED, HARDWARE-VALIDATED 2026-08-02.**
      Boot-to-menu was ~21s; is now ~10-11s. Same total work, just relocated,
      not eliminated.

      **Hard platform trap found and guarded, cost one hardware round**:
      `ref.dllFuncs.R_BeginFrame()` ends in `gEngfuncs.CL_ExtraUpdate()`
      (`engine/client/dll_int/ref_common.c`), which calls
      `clgame.dllFuncs.IN_Accumulate()` **unconditionally**. `clgame.dllFuncs`
      is only populated by `CL_LoadProgs`, at the very end of `CL_Init` -- so
      drawing *any* frame between `VID_Init` and `CL_LoadProgs` jumps through
      a NULL function pointer: unhandled PPU exception, silent freeze, zero
      log output. This means the true pre-video boot window (`FS_Init`,
      `PS3_VerifyGameAssets`, `Sound_Init`, `VID_Init` itself) **cannot be
      drawn into** with the ref API -- would need raw RSX/platform-layer
      drawing before `ref_gl` exists to cover it, not attempted here.
      `SCR_BootProgress` (`cl_scrn.c`, PS3-only) now guards on
      `clgame.dllFuncs.IN_Accumulate` so this trap can't be rediscovered.

      **The real win**: `VOX_PreloadAllWords` (3868 words / 1065 sentences,
      ~11s) was running at boot; moved to `VOX_PreloadDeferred`, called from
      `S_BeginRegistration` (`s_load.c`) at first map load instead, *before*
      `s_registering = true` is set (otherwise `S_RegisterSound` skips the
      `S_LoadSound` that makes the word resident). `SCR_BootProgress` now only
      draws during this deferred preload. Also bumped `FILE_BUFF_SIZE` 2048 ->
      32768 on PS3 (`filesystem_internal.h`) -- weakest of the three changes,
      since `FS_LoadFile` bypasses the buffer entirely for large reads, only
      small sequential reads / in-window `FS_Seek` benefit.

      **Checked and ruled out**: the sibling PSP port
      (`E:\Users\Matteo\Desktop\HL1\PSP\xash3d-fwgs`, branch `psp-cachedfs`)
      eagerly indexes the whole basedir/gamedir into a 5000-entry hash at
      `FS_Init` to dodge `sceIoGetstat` storms -- but xashPS3 is on the newer
      upstream base whose `filesystem/dir.c` already does the same thing
      lazily (cached, sorted `dir_t` tree + bsearch). Porting that trick would
      be duplicate work for ~zero gain; don't revisit.

      **Next lever, cheap, not yet done**: ~5s of the remaining boot time is
      `UI_LoadProgs` failing to find fonts -- six "Unable to read font file
      gfx/fonts/FiraSans-Regular.ttf!" plus one `tahoma.ttf!`, seven missing-
      file searches across every searchpath, every boot. Vendoring the actual
      `.ttf` binaries (goal 7 already wired FreeType2 build-side; only the
      font *files* are missing from `3rdparty/extras/xash-extras/gfx/fonts/`)
      would remove this stall outright and is also goal 7's long-deferred
      cosmetic gap. Second-biggest remaining chunk (~2s) is Sony's own
      `videoConfigure` HDMI mode set -- not ours to fix.

      **`FS_LoadFile` lookup cost -- FIXED and HW-validated 2026-08-03.**
      Map-load filesystem syscall time **12.03s -> 6.78s (-44%)**, measured at
      the same milestone (`miss "gfx/env/desertrt.dds"`, "Setting up
      renderer") across three builds:

      | build | listdir calls/entries/ms | stat calls/ms | total |
      |---|---|---|---|
      | original | 10534 / 38359 / 7518 | 14934 / 4516 | 12.03s |
      | + cache-trust flag | 2044 / 16692 / 4017 | 4386 / 3138 | 7.16s |
      | + targeted invalidation | 2015 / 14858 / 3334 | 4387 / 3442 | 6.78s |

      **The two hypotheses previously recorded here were both WRONG.** They
      are kept only so nobody re-derives them from the old numbers:
      1. "Not cache warmth, cost is deterministic per name" -- false. The
         determinism was an artifact of what dominated at the time. Measured
         directly, `sound/items/smallmedkit1.wav` cost 54.58 / 46.31 / 57.92ms
         *within a single session*. LV2 `stat()` latency is spiky (session
         mean ~0.8ms, outliers 20-58ms), which is also why only >20ms lookups
         trip the profiler print -- a strong selection bias in any log reading.
      2. "Not directory-level" -- false. `populate` fires on exactly the first
         file into each directory and never on the second, which is precisely
         the `launch_select2` (342ms) vs `launch_dnmenu1` (4ms) pair.

      Real causes, both found by instrumenting rather than reasoning:
      - **Hit path cost was `stat()` calls, always `path components + 1`.**
        `FS_FixFileCase` (`filesystem/dir.c`) re-`stat`ed every path component
        to confirm a cached entry still existed, then `FS_FindFile_DIR`
        `stat`ed the file again. Three of four were pure re-validation of what
        `readdir` had already returned.
      - **Miss path re-listed the whole directory, 4x per probe, uncached.**
        `FS_MaybeUpdateDirEntries` ran a full `listdirectory` on every failed
        lookup, on every dir searchpath. One absent VOX word = `scientist/`
        (391 entries) listed 16 times = 130ms.

      Fix: a per-`dir_t` `listed` flag. A directory whose listing is known
      current skips both the per-component re-`stat` and the miss rescan.
      `FS_InvalidateDirCache()` clears the flag on exactly the directories
      written to, walking the cached tree with **no syscalls**, called from
      `FS_Open`'s write branch, `FS_Rename` (both names) and `FS_Delete`.
      Gated by `FS_TRUST_DIR_CACHE` (PS3/PSVITA/NSWITCH); desktop keeps the
      stock defensive rescans, where developers do edit assets live.

      **Trap that cost a hardware round:** the first version used one *global*
      generation counter. Every demo-header/config/save write invalidated the
      entire tree, and each directory then paid a full re-listing at next
      touch -- ~680ms/load hidden behind `populate 0 rescan 0`, invisible
      because `FS_RefreshDirEntries` had no counter. Any new cache path gets a
      counter in the same commit as the code.

      Also settled: **`/dev_hdd0` is case-SENSITIVE**, probed on hardware by
      flipping the case of a real directory entry and `stat`ing it
      (`PS3_ProbeCaseInsensitive`, dir.c, runs once and memoizes). So PS3
      cannot join PSVITA/NSWITCH in bypassing the case-fixing dir cache
      entirely. Do not retry that lever.

      Remaining, both measured and neither part of this path:
      - **~2.25s of boot `listdir`** in one burst before the menu appears
        (`listdir` 15 -> 1918 calls). That is `FS_Search`-driven, a different
        code path, untouched by this work.
      - **~3s of `stat` during VOX preload**: 3867 words x 1 `stat` each, the
        floor of "one existence check per file looked up". Halving it means
        dropping the `FS_SysFileExists` in `FS_FindFile_DIR` when the cache is
        authoritative -- which needs `d_type` carried out of `listdirectory`
        to stay correct about files-vs-directories, so it is not free.

      Instrumentation kept: `[fs/prof]` in `FS_FindFile` (`searchpath.c`)
      prints call mix, worst searchpath and running session totals for any
      lookup over 20ms; counters live in `fs_prof` (`filesystem/sys.c`).

      **PS3 audio feeder emits silence on starvation -- DONE and
      HARDWARE-VALIDATED 2026-08-03.** Load-time audio is now clean silence
      instead of a garbled loop, and gameplay is untouched.

      Root cause, exactly: the feeder thread advanced `snd.samplepos`
      unconditionally every block and never consulted `snd.paintedtime`. Once a
      main-thread stall outlived the ~110ms cushion, the read pointer crossed
      the mix frontier into bytes from one full ring lap earlier (32768 frames
      = 682ms at 48kHz) and lapped forever, splicing at each crossing. The
      artifact was never noise -- it was already-played audio on repeat.

      Fix, entirely inside `engine/platform/ps3/s_ps3.c` (no shared-engine
      surface at all, deliberately, after the failed attempt recorded above):
      consume only `min( margin, 256 )` frames, zero-fill the rest of the
      block, and advance `snd.samplepos`/`ps3_audio_playedFrames` by what was
      really consumed. **The pointer parking is the half that matters** --
      `S_GetSoundtime` derives `snd.soundtime` from `samplepos`, so a stalled
      mixer now sees a soundtime that also stopped and resumes painting exactly
      where it left off. No deadlock: soundtime still advances by whatever *was*
      consumed, so `endtime = soundtime + mixahead` stays ahead of
      `paintedtime` and the mixer refills normally. The unprimed path emits
      silence too, rather than handing out a ring nobody has written yet.

      **Hardware numbers**: 30s of continuous gameplay across six 5s windows at
      16.67-16.82ms (locked 60fps) with **zero** starved blocks; starvation
      confined to loads (first map load ~4.4s, c0a0->c0a0a transition 1.60s =
      301 blocks x 5.33ms, matching the frame counter exactly); `0 resyncs` all
      session. Menu audio confirmed unaffected.

      **Expected behaviour, not a bug**: audio keeps playing for ~1s after a
      load begins, then falls silent. Frames still complete early in a load
      (measured `avg 155ms` / `avg 32.98ms` windows), and each one runs the
      mixer; the silence starts when the genuinely synchronous stretch begins
      (`SV_SpawnServer` -> `CL_PrecacheResources` -> `S_EndRegistration`, no
      `Host_Frame` at all -- `client 360-379ms` per frame in the same log).

      **Instrumentation trap worth knowing**: the first version reported a
      "worst single-block deficit", which is saturated by construction --
      `real` is clamped to one block, so it pins at 256 the first time the
      mixer stops outright and reads a constant for the rest of the session. It
      also proved `real` is always 0, never partial: the mixer does not
      trickle, it stops dead and comes back. Replaced with the longest unbroken
      run of silent frames, which is what separates one long load stall from
      chronic micro-starvation during play.

      This does NOT shorten loads or keep audio playing through them -- only a
      running mixer can do that, and the attempt at that is the reverted item
      above. It makes exhausting the cushion graceful instead of ugly.

      **First-use client effect hitch eliminated -- DONE and HARDWARE-
      VALIDATED 2026-08-08.** Breaking a wood box reproduced the reported
      effect-only stutter and the existing diagnostics identified the exact
      synchronous load: `debris/wood4.wav` took 364.55ms (364.42ms I/O,
      0.13ms decode), producing a 385.41ms frame and 49 newly starved audio
      blocks. The diagnostic build contained no fix; this trace established
      that the hitch was asset loading on the main thread, not distortion or
      a feeder-thread timing problem.

      Root cause: the engine's client temp-entity `SoundList` contains effect
      variants that the HLSDK server does not always precache (for example,
      `BounceWood` has `wood1` through `wood4`, while `CBreakable` registers
      only `wood1` through `wood3`). The first random selection of an omitted
      variant called `S_RegisterSound` while the game was active, forcing
      `S_LoadSound` synchronously; later uses were smooth because the sample
      was then cached.

      Fix in `engine/client/sound/s_load.c`: immediately after
      `S_BeginRegistration` sets `s_registering = true`, a PS3-only pass
      registers every client effect entry from `BouncePlayerShell` through
      `Explode` (38/38 with the default list, including custom `sounds.lst`
      replacements). Existing sound hashing deduplicates server-registered
      entries, and `S_EndRegistration` performs the actual I/O during map
      loading. **Hardware result: `SUCCESS, no stutter`.** Cockroach squish,
      another effect that previously produced the same first-use hitch, was
      also explicitly confirmed smooth; validation is therefore not limited
      to the original wood-box reproducer. The audio feeder, thread priority,
      mixahead, interpolation, HLSDK behavior, and asset packaging were
      deliberately left unchanged.

      **Pumping `S_ExtraUpdate()` through the blocking load paths -- TRIED
      2026-08-03, made the stutter WORSE on hardware, REVERTED.** Do not
      re-attempt it in the shape described below without new evidence.

      What was built (all reverted, `git checkout` clean, nothing left in the
      tree): a new `S_ExtraUpdateLoading()` next to `S_ExtraUpdate` in
      `s_main.c`, called once per loaded item from `S_EndRegistration`'s
      "load everything in" loop (`s_load.c`), `CL_PrecacheResources`' three
      loops (`cl_main.c`), `VOX_PreloadDeferred` (`s_vox.c`) and
      `SV_LoadFromFile`'s entity loop (`sv_game.c`, `#if !XASH_DEDICATED`).
      Plus three supporting changes in `S_UpdateChannels`: a mixahead
      parameter so load pumps could mix 0.25s ahead instead of the cvar's
      0.12, `SOUND_DMA_SPEED` -> `SOUND_OUTPUT_SPEED` on the mixahead line
      (the cushion is a duration; at 48000 output the cvar buys 110ms not
      120), and a re-entrancy guard (`S_PaintChannels` reaches `S_LoadSound`
      and `VOX_LoadWord` via `s_mix.c:351`/`298`).

      **Two things worth keeping from the analysis, both verified by reading
      the code and independent of the failed fix:**
      1. `S_RegisterSound` does **not** touch disk while `s_registering` is
         true (`s_load.c:370`, `if( !s_registering ) S_LoadSound( sfx )`).
         Every map sound is actually loaded later, in one loop inside
         `S_EndRegistration`. Any future work on level-load sound cost belongs
         there, not in `CL_PrecacheResources`' precache loop.
      2. A channel left pointing at a freed sfx is already handled: `S_FreeSound`
         memsets the name, `S_LoadSound` returns NULL for it, and
         `S_MixNormalChannelsToRoombuffer` frees the channel. Painting during
         registration is not a crash risk, so that is not what went wrong.

      **Process failure worth not repeating: this went to hardware as one
      bundled change** -- six call sites plus three behaviour changes to the
      mixer -- so the regression is not attributable to any single part. The
      deeper 0.25s mixahead and the extra `S_LoadSound` work the pumps
      themselves trigger (each pump can lazily load an active channel's sfx,
      i.e. it adds disk I/O to the very loop it is trying to protect) are both
      untested suspects, not findings. Section 2's rule applies: one variable
      per hardware round.

      Still declined by the user: packing `valve/` into `pak0.pak` (currently
      a loose-file tree, see section 3 above) -- the biggest single structural
      lever for both boot and map-load time, but out of scope by user choice.
      The `FS_LoadFile` item above was fixed without touching this, so packing
      is no longer needed to close it; re-confirm the preference before
      proposing it for anything else.
- [x] **15. Boot cinematic (Valve/Sierra intro video)** -- DONE and
      HARDWARE-VALIDATED 2026-08-03 (pulled forward out of order at user
      request, ahead of goal 14). The intro plays on boot with picture and
      sound at correct pitch. The cinematic state machine
      (`engine/client/cl_video.c`) is stock upstream logic as predicted; the
      decoder was the whole job.

      **The three prerequisites recorded here on 2026-07-27 were all wrong,
      and they were wrong because they assumed ffmpeg was the only way to
      decode an AVI.** Reading the actual asset headers settled it in one
      pass: stock Half-Life media is `logo.avi` = Microsoft RLE 8-bit
      640x100 (no audio) and `valve.avi` = Cinepak 640x480 + 22050 Hz mono
      8-bit PCM. Both are small, fully documented, integer-only codecs. So
      no ffmpeg cross-compile (prereq 1), no decode-cost problem -- Cinepak
      at 640x480/15fps is trivial for the PPE (prereq 2), and prereq 3 was
      right but irrelevant (the user's own HL data supplies the files).
      **Lesson: identify the codec before scoping the decoder.**

      **Shipped**: a third backend, `AVI_NATIVE` (`common/backends.h`,
      selected for PS3 in `common/defaults.h`), implemented in
      `engine/client/avi/avi_native.c` (RIFF demux, whole file into RAM,
      frame pacing, PCM into a raw sound channel) plus `avi_cinepak.c` and
      `avi_msrle.c`. `avi_ffmpeg.c` is wrapped in
      `#if XASH_AVI != AVI_NATIVE` -- its shared tail cannot be hoisted into
      a common TU because it declares `static movie_state_t avi[2]` and so
      needs the complete struct.

      **Three traps this cost, all worth knowing elsewhere:**
      1. **`fopen` cannot open the paths `FS_GetDiskPath` returns.** They
         are relative to the working directory (`valve/media/valve.avi`) and
         PS3 stdio fails on them, while every other loader in the engine
         works because it goes through `FS_SysOpen`/`open`. Use
         `FS_LoadDirectFile( path, &len )` for any disk-path load. Cost one
         hardware round.
      2. **All raw-channel audio was 8.84% sharp (~1.5 semitones), engine-wide
         and pre-existing.** Normal channels resample against
         `snd.format.speed` (`s_mix.c:312`), but `S_RawSamplesStereo` and the
         feed-rate estimates used the hardcoded `SOUND_DMA_SPEED` (44100)
         while this port opens the device at 48000 -- so movie audio *and the
         mp3 background music* had been transposed since goal 9. Fixed with
         `SOUND_OUTPUT_SPEED` (`engine/client/sound.h`). Deliberately not
         touched: `s_mixahead * SOUND_DMA_SPEED` (`s_main.c:1550`), the
         ~110ms cushion tied to the underrun behaviour tracked under goal 14.
      3. **Cinepak strips past the first inherit the previous strip's
         codebooks on a keyframe** (`!(frame_flags & 0x01)`); they ship only
         selective updates (chunks 0x2100/0x2300). Omitting it leaves a
         recognizable image sprinkled with stale blocks -- not a desync, byte
         consumption is exact either way.

      **Method worth reusing: the decoder bug above cost zero hardware
      rounds.** The real `avi_native.c`/`avi_cinepak.c`/`avi_msrle.c` were
      built for x86-64 Linux inside the ps3dev Docker image against a ~90-line
      engine shim (types, `Mem_*` -> malloc, a fake `ref.dllFuncs`, a
      harness-controlled `Platform_DoubleTime`), run under
      `-fsanitize=address,undefined` on the real `logo.avi`/`valve.avi`, with
      decoded frames dumped to PPM and converted to PNG for eyeballing.
      Running on a little-endian host also proves no raw struct casts crept
      into the parsers. Any future codec/parser work on this port should
      start there, not on the console.

      **Data-side, no code needed**: Steam-era HL ships
      `media/StartupVids.txt` containing `media/valve.webm` (VP8, not
      decodable here) with `valve.avi` sitting next to it, so
      `SCR_PlayCinematic` now retries the same name with `.avi` before
      failing. The `AVI: ...webm is not a supported AVI file` line on every
      boot is that first attempt and is expected. A failed movie also used to
      kill the whole playlist (`cls.state` never reaches `ca_cinematic`, so
      `SCR_RunCinematic` never gets back to `SCR_NextMovie`); `cl_cmds.c`
      now advances instead.

      **Not the backend's problem, do not debug it as one**: the animated
      menu logo (`logo.avi`, `UI_DrawLogo`) is gated by mainui on the WON
      background only (`s_bEnableLogoMovie`,
      `3rdparty/mainui/controls/BackgroundBitmap.cpp:337`). With the HD/Steam
      background it stays off regardless of the decoder; it needs
      `ui_prefer_won_background 1`. The MSRLE path itself is verified
      correct off-target but has not yet been seen on hardware.

- [ ] **16. GoldSrc flashlight/dynamic-light composition -- SHELVED
      2026-08-08 at user request. Do not resume without an explicit user
      request.** The flashlight works logically and its dynamic lightmap moves
      with the view, but on PS3 the final lightmap pass is composed incorrectly:
      toggling the flashlight changes the scene, yet it does not produce the
      correctly localized illumination visible in the PC reference build.
      This is not a shadow-casting system: the HL client traces forward and
      places a small point dlight at the hit position; `ref_gl` marks affected
      BSP surfaces, rebuilds their luxels into the dynamic lightmap atlas, and
      blends that atlas over the already-rendered base textures.

      **What hardware evidence established:** gameplay telemetry repeatedly
      showed nonzero dlighted surfaces/luxels and continuous `TexSubImage2D`
      uploads while the flashlight was active. Dumps such as
      `ps3_dlight_431_00.tga`, `ps3_dlight_207_00.tga`, and
      `ps3_dlight_121_00.tga` showed the generated atlas content; the moving
      result and replacement-mode probes established that the dynamic atlas and
      surface coordinates reach the draw. The PC build at the same viewpoint
      composes the pass correctly. The remaining unresolved boundary is final
      framebuffer composition in the PS3 GL-to-RSX path.

      **Ruled out on real hardware, do not repeat without new evidence:**

      1. Texture-cache coherency alone: PS3GL now tracks texel writes separately,
         orders PPE stores with `sync`, and emits one
         `rsxInvalidateTextureCache` before sampling an updated bound texture;
         the flashlight was unchanged.
      2. An in-flight atlas rewrite: a PS3-only diagnostic `pglFinish` fully
         serialized pending draws before `LM_UploadDynamicBlock`; unchanged.
      3. Atlas generation/placement and a simple big-endian upload failure:
         CPU atlas dumps, byte-layout probes, and replacement-mode rendering
         did not account for the faulty final appearance.
      4. ~~VBO selection and overbright policy~~ -- **HALF OF THIS ENTRY WAS
         WRONG, see the 2026-08-08 round below.** `gl_vbo 1` making the image
         worse still stands. The claim that forcing `gl_overbright 0` did not
         correct it does NOT: that cvar could not be set at all at the time, so
         the test never ran.
      5. Deferred `GL_POLYGON` batching: the diagnostic build submitted every
         polygon immediately. During actual gameplay it logged `1.0
         passes/frame`, `0.0 GL_POLYGON draws merged/frame`, 10--12 dlighted
         surfaces/frame, and 450--871 draw flushes/frame; the flashlight was
         still unchanged. The temporary no-batching source switch was reverted
         when this goal was shelved. Note this exoneration was measured on a
         build with batching *disabled*; the shipping build batches 500--700
         polygons/frame, so it does not automatically carry over.

      **2026-08-08 round -- resumed at user request. The blend hypothesis is
      dead; one real bug was found and fixed on the way.**

      A five-mode isolation cvar (`gl_ps3_dlight_probe`, replacing the
      confounded `gl_ps3_dlight_replace`) plus `ps3gl_log_draw_state()` in
      `3rdparty/ps3gl/ps3gl_states.c` dump the state the lightmap passes
      actually composite under. The old probe moved blending, texenv AND vertex
      colour at once -- and on PS3GL the texenv selects a different *fragment
      program* (`q3_fp_modulate` = `color*tex` vs `q3_fp_replace` = `tex`), so
      "replace looks right, normal looks wrong" never implicated the blend unit
      specifically. That is why five rounds did not converge.

      What the probe proved on hardware:

      - **`gl_overbright` was 1, on a build declaring it "0".** The pass logged
        `src=0x0306` (`GCM_DST_COLOR`) and `color=aaaaaaff` (= 128/192), both
        reachable only from the overbright branch. Cause: `FCVAR_READ_ONLY`
        cannot defend an `FCVAR_GLCONFIG` cvar, because `opengl.cfg` replay goes
        `Cvar_SetGL` -> `Cvar_FullSet` -> `Cvar_DirectFullSet`, which never calls
        `Cvar_CanSet`. **Fixed** by dropping `FCVAR_GLCONFIG` on PS3 (see the
        comment on the declaration in `ref/gl/gl_opengl.c`). Turning it off
        improved the image but did not fix the flashlight.
      - **With overbright off, every state field matches the PC reference
        exactly**: `GL_ZERO/GL_SRC_COLOR`, texenv MODULATE, `fp_key=1`
        (MODULATE), `tmu1` disabled, vertex colour `ffffffff`, depth `GL_EQUAL`
        with writes off, alpha test off. This kills the RSX-blend-mismatch
        thesis, the stale-`glColor` thesis, and the "both TMUs bound so
        `ps3gl_shader_key()` silently picks MODULATE2" thesis.
      - **The dynamic atlas content is correct.** `gl_ps3_dump_dlight 1`
        produced a 128x11 RGBA page that is 60% distinctly warm lightmap data
        with saturated highlights and only 1.4% near-neutral texels. Wrong
        texcoords would therefore still land on warm pixels.
      - Uninitialized VRAM is NOT in play: `glTexImage2D` with `pixels == NULL`
        memsets to zero in `ps3gl_textures.c`, so the 117 never-uploaded rows of
        the 128x128 `tr.dlightTexture` are black, not garbage.

      **Do not build `tools/ps3_blendtest`** (the old resume point). The blend
      state was exonerated from inside the real pipeline, which is strictly
      better evidence than a synthetic panel test.

      **Resume point:** every stage from luxel to framebuffer is now
      individually verified correct while the composite is still visibly wrong,
      so single-sided probing has run out. Get a like-for-like PC reference --
      same map, same viewpoint, `gl_overbright 0`, screenshots at `r_lightmap 0`
      and `r_lightmap 1`, plus a PC atlas dump if available -- and diff against
      the PS3 at the same spot. Note when reading `r_lightmap 1` that it only
      disables the *blend*; the base-texture pass still draws, so flat grey
      faces in that mode may just be surfaces absent from any lightmap chain,
      not artifacts. Three separate misreadings of gameplay stills happened in
      this round; insist on the reference comparison instead.

Work ONLY the top unchecked item. Do not implement future items speculatively.

## 4. Platform conventions

- `engine/platform/ps3/` is the *only* platform-specific code location --
  this reuses Xash3D-FWGS's own existing plugin convention (`platform/<os>/`),
  not a parallel `code/ps3/` tree.
- **`ref_gl` is the default renderer; `ref_soft` stays as the `-ref soft`
  fallback.** Both are statically linked. `DEFAULT_RENDERERS`
  (`engine/client/dll_int/ref_common.c`) already lists `"gl"` first, so the
  engine picks GL unless told otherwise.
  - `ref_gl` runs on **`3rdparty/ps3gl`**, a GL 1.1 fixed-function subset over
    the RSX ported from the sibling `ioQuake3-PS3` port's `code/gl/`. It is
    linked with `XASH_GL_STATIC=1` -- ps3gl defines the real `gl*` symbols, and
    ref_gl calls them directly, the same shape PSVita uses with vitaGL. There is
    no dynamic loader on PSL1GHT, so `GL_GetProcAddress` can never resolve
    anything and the function-pointer path is not an option.
  - The old "no ref_gl is possible" claim was wrong. PSL1GHT still has no
    *runtime* shader compiler, but ps3gl doesn't need one: its Cg vertex and
    fragment programs are compiled offline with `cgcomp` and checked in as
    `.vpo`/`.fpo` under `3rdparty/ps3gl/shaders/`, embedded through the
    generated `ps3gl_shader_data.h`. The build itself never invokes cgcomp;
    regenerate only when a `.vcg`/`.fcg` source actually changes.
  - **ps3gl links into the ENGINE, not into ref_gl**, even though ref_gl is its
    only GL consumer. `--static-linking` merges ref_gl into one relocatable
    object and runs `objcopy -G lib_ref_gl_exports`, which localizes everything
    but `GetRefAPI` -- so anything defined inside `ref_gl.o` is unreachable from
    the engine. `vid_ps3.c` owns the gcm context and the flip loop and must call
    `ps3gl_init`/`ps3gl_begin_frame`/`ps3gl_end_frame` itself, so ps3gl has to
    sit on the engine's side of that boundary. ref_gl's `gl*` calls stay
    undefined in `ref_gl.o` and resolve at the final link.
  - `ref_soft` renders into a CPU-writable XDR buffer that `SW_UnlockBuffer`
    transfers to an RSX display buffer and flips; in that mode the RSX is
    presentation-only (no render target, no depth buffer). `ref_gl` needs both,
    so `R_Init_Video` allocates a Z24S8 depth buffer and calls `rsxSetSurface`
    only on the `REF_GL` path.
- **No SDL anywhere in this port.** Unlike `nswitch`/`psvita` (which still
  compile `platform/sdl2/*.c` under the hood), PS3 has no SDL2 port to lean
  on. The closest *genuinely* SDL-free reference in this tree is
  `engine/platform/dos` -- model new platform files on that, not on
  nswitch/psvita.
- **`LAUNCHER=False`, static single ELF.** PS3 is in the `wscript` DEST_OS
  exclusion list for `LAUNCHER`, so the build defines `XASH_ENABLE_MAIN=1`
  and `engine/common/launcher.c` supplies `main()` -> `Host_Main()` directly.
  No `game_launch/` involvement needed.
- **PS3 behaves like POSIX for almost everything.** It is *not* in any of
  the psvita/nswitch exclusion lists in `filesystem/sys.c`/`dir.c` (real
  `dup()`, real large-file support) and it automatically falls into the
  generic `else` branches of `wscript`'s large-file check and `engine/wscript`'s
  POSIX lib list. Resist the urge to add PS3-specific carve-outs unless a
  real build error demands one.
- **Functions already provided "for free" once `platform/posix/*.c` compiles
  for PS3** (confirmed by reading the actual guard conditions, not assumed):
  `Platform_DoubleTime`, `Platform_Sleep`, `Platform_ShellExecute` all come
  from `engine/platform/posix/sys_posix.c` (its `Platform_ShellExecute` guard
  is `#if !XASH_ANDROID && !XASH_NSWITCH && !XASH_PSVITA` -- PS3 is not
  excluded). `Platform_MessageBox` comes from `engine/common/sys_con.c`'s
  `MSGBOX_STDERR` fallback. **Do not redefine any of these in `sys_ps3.c`** --
  duplicate symbols will fail to link.
- **Functions that are dead code for PS3, don't implement them**:
  `Platform_SetStatus` (only called under `XASH_PLATFORM_HAVE_STATUS`, which
  is `XASH_WIN32 || XASH_LINUX` only), `Platform_DebuggerPresent` (only
  called via `Sys_DebuggerPresent()`, guarded `XASH_LINUX || XASH_WIN32`),
  `Platform_GetDisplayOrientation` (only called from gyro code gated
  `#if XASH_SDL`).
- PSL1GHT libs are injected once, globally, via the toolchain class's
  `ldflags()` in `scripts/waifulib/xcompile.py` (`-lrsx -lgcm_sys -lio
  -laudio -lsysutil -lsysmodule -lnet -lnetctl -lrt -llv2 -lm`) -- do not
  duplicate them in `engine/wscript`'s per-target lib list.

## 5. Platform detection (`XASH_PS3` macro)

Contrary to first assumption, `XASH_<PLATFORM>` macros are **not** generated
by waf/`conf.define()` -- they come from a plain, compiler-predefined-macro
detection header at `3rdparty/library_suffix/include/build.h` (a submodule
that was previously *not* checked out in the vendor source and had to be
`git submodule update --init`'d during this scaffolding session). This
project has already added:

- `build.h`: `#elif defined __PPU__` -> `#define XASH_PS3 1`, nested in the
  "POSIX compatible" branch (same place as `__vita__`/`__SWITCH__`), which is
  why PS3 automatically gets `XASH_POSIX` too.
- `buildenums.h`: `PLATFORM_PS3 8` (reused the reserved slot) +
  `XASH_PLATFORM` mapping.
- `library_suffix.c`: `Q_PlatformStringByID` case for `PLATFORM_PS3`.

**Open verification item**: `__PPU__` is the well-known PSL1GHT/ps3toolchain
GCC predefine for the PPU side, but this has not been confirmed against the
user's actual installed compiler in this session (no toolchain available
here). Before goal-stack item 1, run
`powerpc64-ps3-elf-gcc -dM -E - < /dev/null | grep -i ppu` (or `ppc`) and
adjust the `#elif defined __PPU__` line in `build.h` if the real macro
differs.

Endianness is **fully automatic** -- `build.h`'s endianness block already
handles `__BIG_ENDIAN__`/`__BYTE_ORDER__` detection generically, and a real
big-endian PPU GCC will define these correctly. No toolchain-side
`XASH_BIG_ENDIAN` forcing is needed.

## 6. Known blockers / open questions

- **`__PPU__` macro name: VERIFIED.** Confirmed directly against the real
  toolchain (`C:\devkitPro\msys2\opt\ps3dev`, `powerpc64-ps3-elf-gcc` 7.2.0):
  `ppu-gcc -mcpu=cell -dM -E - </dev/null | grep -i ppu` defines `__PPU__ 1`.
  `build.h`'s `#elif defined __PPU__` check is correct as written, no change
  needed.
- **ABI: genuinely LP64, NOT ILP32 -- the earlier assumption here was
  wrong, corrected 2026-07-16 against the real compiler.** The same `-dM -E`
  dump also shows `__powerpc64__ 1`, `__PPC64__ 1`, `__LP64__ 1`, `_LP64 1`,
  `__SIZEOF_POINTER__ 8`, `__SIZEOF_LONG__ 8` -- this toolchain compiles
  8-byte pointers and 8-byte `long` by default (matches the `powerpc64-`
  triple name literally). `build.h`'s existing `XASH_64BIT` detection
  (keyed on `__PPC64__`/`__powerpc64__`) is therefore **correct as-is** for
  this platform -- do NOT add a PS3 carve-out to force it off. A minimal
  standalone link+`ppu-readelf -h` also confirmed the produced binaries are
  genuinely `ELF64`, big-endian, `Machine: PowerPC64`. Any pointer-size-
  sensitive engine code (packed structs, save-file formats, anything that
  assumed a 4-byte pointer) needs auditing against 8-byte pointers once
  goal 2 starts -- this is a real, newly-surfaced risk area, not a closed
  item.
- **`sfo.py`/`pkg.py`/`make_fself` exact CLI flags** in
  `scripts/waifulib/ps3.py` are a best-effort shape, not verified against the
  user's installed ps3toolchain revision -- run `--help` on each before
  relying on the packaging chain.
- **`PS3DEV`/`PSL1GHT` env layout** assumed by `scripts/waifulib/xcompile.py`'s
  `PS3` class (`$PS3DEV/ppu/bin/powerpc64-ps3-elf-*`,
  `$PS3DEV/ppu/include`, `$PS3DEV/portlibs/ppu/include`) -- adjust if the
  user's install differs.
- **Main RAM budget**: expect roughly 190-213MB of usable XDR RAM after OS
  reservations, not the full 256MB -- probe with a malloc-until-fail test at
  boot once goal-stack item 2 is reached, don't assume a number.
- Audio (`s_ps3.c`) and video (`vid_ps3.c`) RSX/audio-port bodies are
  deliberately left as TODOs referencing the goal-stack item that will
  implement them -- this is goal 0 (scaffolding), not goal 4/5/7.
- **hlsdk-portable feasibility (this was goal 10, build-verified 2026-07-25
  -- see the goal-10 entry above for the real implementation; this note is
  kept as the original pre-implementation analysis)**: `hlsdk-portable`
  (sibling repo, `E:\Users\Matteo\Desktop\HL1\hlsdk-portable`) has no PS3/
  `__PPU__`/PSL1GHT code today (verified by full-tree grep -- one irrelevant
  hit, an SDL2 controller-name string). What it DOES already have, verified
  by reading the actual files:
  - `common/byteswap.h:87-103` -- real, working `XASH_BIG_ENDIAN`-keyed
    `LittleToHost`/`BigToHost` swap macros, with genuine consumers in
    `dlls/util.cpp` (`CSave`/`CRestore` save-game serializer, ~2096-2403) and
    `dlls/nodes.cpp` (`CGraph::Byteswap*`, ~2400-3675, AI node-graph binary
    format). `cl_dll/parsemsg.cpp:67-124` network message reads are
    endian-safe by construction (byte-at-a-time, no raw multi-byte cast).
    Directly useful precedent for goal 8 (endianness audit).
  - `public/build.h:207-211` already has `#elif defined __PPC__ ||
    defined __powerpc__` -> `XASH_PPC 1` with 64-bit detection via
    `__PPC64__`/`__powerpc64__` -- matches this project's confirmed real PPU
    predefines (section 6 LP64 finding) with zero changes needed.
  - **DONE 2026-07-19 (prep only, harmless -- hlsdk-portable isn't vendored
    into xashPS3 yet, this doesn't touch xashPS3's own build)**: added the
    `#elif defined __PPU__` -> `#define XASH_PS3 1` OS-detection branch to
    hlsdk-portable's own `public/build.h`, mirroring
    `3rdparty/library_suffix/include/build.h`'s existing PS3 branch exactly
    (same nesting inside the POSIX-compatible `#else`, next to
    `__vita__`/`__SWITCH__`), plus the matching `#undef XASH_PS3` in the
    undef list at the top of that header.
  - `cl_dll/studio_util.cpp` NEON SIMD blocks are all `#if XASH_ARMv8` with
    a portable scalar `#else` fallback -- PPC compiles through the fallback
    with zero porting effort; AltiVec could be added later as an optional
    perf path using the same guard pattern.
  - **The real blocker**: `dlls`/`cl_dll` build as CMake `SHARED` libraries
    by default, with a single dynamic entry point, `GiveFnptrsToDll`
    (`dlls/h_export.cpp:52`, plain `extern "C"` export, no static-
    registration alternative). PSL1GHT has no `dlopen`. **Correction found
    during goal 10's real implementation**: hlsdk-portable actually does
    ship its own waf build too (`wscript`, `dlls/wscript`, `cl_dll/wscript`,
    written for the Xash3D-FWGS ecosystem, with existing PSVita/NSwitch
    `DEST_OS` branches) -- CMake is not the only build system, contrary to
    what this note originally assumed. That waf build is still standalone
    (produces a loadable `.so`, not designed to be pulled into a different
    parent project's static link), so new simplified wscripts were written
    for the static-linking case rather than reusing it verbatim, but it
    was a far better reference than starting from the CMakeLists.
    No upstream platform (Android/PSVita/NSwitch) has ever applied
    `--static-linking` to the actual game DLL -- PSVita/NSwitch use
    dlopen-compatible shims (VRTLD/SOLDER) instead. PS3 is the first
    to statically link game logic this way, not just engine-internal
    modules -- confirmed as real, non-copy-paste adaptation work by goal
    10's build (see the goal-10 entry above for the five real bugs found
    getting it to link).

## 7. Build & deploy commands

```
./waf configure --ps3 --static-linking=filesystem_stdio,ref_soft,ref_gl,menu,server,client
./waf build
```

(`--disable-mbedtls`, present in every historical build line quoted in the
goal entries above, is **gone as of the 2026-08-04 repo cleanup** -- that
option was declared by `3rdparty/mbedtls/wscript`, and the whole subproject
was removed since its submodule was never vendored here and the engine never
linked it. Passing the flag now fails with "no such option"; drop it.)

(`--dedicated=no` from an earlier draft of this doc doesn't actually parse
with this waf version -- `-d`/`--dedicated` is a plain flag, not a
`key=value` option. `--static-linking` lists every module this project
currently statically links into `xash` -- `filesystem_stdio`/`ref_soft`
since goal 2, `menu` (`3rdparty/mainui`, target name `menu` per its own
`wscript`) since goal 7, `server`/`client` (`hlsdk-portable/dlls`/
`hlsdk-portable/cl_dll`, the real game DLL, target names `server`/`client`
per their own `wscript`s -- note these are the waf `name=` values, which is
what `xshlib.py` actually matches against, not the `target=` string) since
goal 10, replacing the old goal-6 `client` stub, and `ref_gl` since goal 11;
extend this list as future goals add more. Both renderers are listed on
purpose -- `ref_gl` is the default and `ref_soft` remains reachable with
`-ref soft`, which is the only way to A/B a rendering bug against a
known-good path on hardware where there is no debugger.)

Output: `engine/xash` ELF -> (via `scripts/waifulib/ps3.py`) `EBOOT.BIN` +
`PARAM.SFO` + a `.pkg`, staged under `build/engine/pkg/` (`PARAM.SFO` and
`ICON0.PNG` at its root, `EBOOT.BIN` under `USRDIR/`).

**Flavors.** Adding `--gamedir=<mod>` at configure time selects a mod flavor,
which changes the XMB title, the 9-character `TITLE_ID` and the icon. The
mapping lives in the `PS3_FLAVORS` table at the top of the root `wscript`;
configure fails loudly on an unknown gamedir, a `TITLE_ID` that isn't exactly
9 characters, or a missing icon file. Known flavors:

```
./waf configure --ps3 --static-linking=...                    # valve   -> XASH10000, build/
./waf configure --ps3 --gamedir=bshift --static-linking=...   # bshift  -> XASHBS000, build_bs/
./waf configure --ps3 --gamedir=gearbox --static-linking=...  # gearbox -> XASHOF000, build_opfor/
./waf configure --ps3 --gamedir=ricochet --static-linking=... # ricochet-> XASHRC000, build_ricochet/
./waf configure --ps3 --gamedir=cstrike --static-linking=...  # cstrike -> XASHCS000, build_cs/
```

`ricochet` currently carries **no game code of its own** -- it builds base HL1
sources under its own gamedir, which boots to the menu but is not the mod. Its
path forward is described with `RICOCHET` in `PS3_GAME_DEFINES`.

`cstrike` is different in kind. Counter-Strike is not an hlsdk-portable variant,
so the flavor **swaps whole source trees** instead of gating one with a define.
Two pairs of `SUBDIRS` rows do it, and in each pair exactly one row supplies the
waf `name=` that `xshlib.py` links:

- `name='client'` -- `cs16-client/cl_dll` when `PS3_GAME == 'cstrike'`,
  `hlsdk-portable/cl_dll` otherwise.
- `name='menu'` -- `3rdparty/mainui_cs` when `PS3_GAME == 'cstrike'`,
  `3rdparty/mainui` otherwise.
- `name='server'` -- `regamedll/dlls` when `PS3_GAME == 'cstrike'`,
  `hlsdk-portable/dlls` otherwise.

`cs16-client/` is a vendored subset of Velaron/cs16-client (client only:
`cl_dll/` minus its dead `VGUI/` and `hl/` dirs, plus that SDK's own `common/`,
`engine/`, `public/`, `pm_shared/`, `game_shared/` and `dlls/` headers +
`wpn_shared/`).

`regamedll/` is Velaron/ReGameDLL_CS at `d1af136` (the commit cs16-client pins
as its own `3rdparty/ReGameDLL_CS` submodule), vendored from that repo's
`regamedll/` subdirectory: `dlls/`, `game_shared/`, `pm_shared/`, `public/`,
`engine/`, `common/`, the inner `regamedll/`, and `version/`. 156 translation
units against base HL1's 41. Things worth not re-deriving:

- **A recursive `**/*.cpp` glob is correct here**, unlike `cs16-client/cl_dll`
  where whole directories are dead code. Every `.cpp` upstream's CMake does not
  list was deleted at vendoring time, so glob == CMake list. The one trap:
  `regamedll/public_amalgamation.cpp` `#include`s `common/stdc++compat.cpp`, so
  that file is absent from the CMake source list yet must stay on disk --
  deleting it as "unused" breaks the build.
- **The bots cannot be separated out.** `dlls/bot/` (36 TUs) is referenced from
  11 core files with no build-time guard, including a `static_cast<CCSBot *>`
  in `player.cpp` and `TheCSBots()` in `cbase.cpp`/`weapons.cpp`/`gamerules.cpp`,
  so excluding it means stubbing core game code. They are also the only offline
  opponents possible here: cs16-client's other bot, YaPB, is a metamod plugin
  and PSL1GHT has no dlopen.
- **No SSE reaches the compiler.** `regamedll/sse_mathfun.cpp` and the SSE arms
  of `common/mathlib.h` are gated on `HAVE_SSE`, which `engine/osconfig.h` only
  defines when `REGAMEDLL_SSE` is set *and* an `__SSE__`-class macro is present.
  The wscript defines neither and skips upstream's `-msse3`. `public/asmlib.h`
  (Agner Fog's x86 asm library) is declarations only and its `Q_*` macros are
  behind `HAVE_OPT_STRTOOLS`, also undefined.
- **This module does not use the `werror` uselib.** waf emits a taskgen's own
  flags *before* a uselib's, so the tree's upstream `-Wno-write-strings`,
  `-Wno-strict-aliasing` et al could never override `-Werror=` versions of the
  same. The wscript carries upstream's warning set instead.
- Same `-fno-rtti` / `-mminimal-toc` / `-fno-strict-aliasing` reasoning as the
  CS client below, plus upstream's `-fno-exceptions`, `-fno-builtin`,
  `-fno-sized-deallocation` (entities override `operator new`/`delete` to
  allocate through `ALLOC_PRIVATE`; C++14 sized deallocation would route to an
  overload they do not implement) and `-fno-devirtualize` (the ReGameDLL API's
  hookchains assume real virtual dispatch).
- **`XASH_64BIT` must be defined for this module, and it is not optional.**
  `dlls/qstring.h` selects the game's `string_t` representation with
  `#if XASH_64BIT`: the 64-bit arm keeps a signed int offset into the engine's
  string pool, the 32-bit arm casts pointers straight to `unsigned int`. That
  macro is only ever set by `public/build.h` -- which **nothing in this SDK
  includes** -- so without the wscript define the 32-bit arm compiles on an LP64
  platform and every classname/targetname round-trips through a truncation,
  with any negative pool offset becoming ~+4GB. The symptom on hardware was a
  silent freeze inside `CWorld::Precache` on the first map load. hlsdk-portable
  is immune only because its `dlls/extdll.h` includes `build.h`, whose
  `__LP64__` auto-detect sets the macro for free. General lesson: patching a
  vendored `build.h` proves nothing until something actually includes it.
- `dlls/exports.txt` is a standalone 210-line file (4 entry points + 206
  entity classnames), not an `exports_cstrike.txt` add-on: `apply_xshlib` reads
  `exports.txt` from the taskgen's own directory. ReGameDLL defines
  `GiveFnptrsToDll`, `GetEntityAPI`, `GetNewDLLFunctions` and
  `Server_GetBlendingInterface` -- there is no `GetEntityAPI2`, which is fine,
  `sv_game.c` falls back to `GetEntityAPI`.

Patches inside `regamedll/`, all documented in place. Three of them are new
classes of PS3 problem, not repeats of the CS client's:

- `public/FileSystem.cpp`: `FileSystem_Init` dlopens `filesystem_stdio`. On PS3
  that module is statically linked into the engine and exports
  `CreateInterface`, so the PS3 branch asks the engine's own native-object
  registry instead -- `Sys_GetNativeObject("VFileSystem009")` ->
  `FS_GetNativeObject` -> filesystem_stdio's `CreateInterface`. The two
  `IFileSystem` declarations (this SDK's `public/FileSystem.h` and the engine's
  `filesystem/VFileSystem009.h`) are the same stock Valve layout member for
  member, so the vtable slots line up; `FileExists` is the only method this tree
  calls, from three unguarded sites in `hostage.cpp`/`cs_bot_chatter.cpp`.
- `engine/osconfig.h`: PPU newlib has no `dlfcn.h`, `elf.h`, `link.h`,
  `pthread.h`, `sys/ioctl.h`, `sys/mman.h` or `sys/sysinfo.h`, so the POSIX
  include block gets a `__PPU__` arm, and the unused `ioctlsocket`/`sys_allocmem`
  /`sys_freemem` inlines are compiled out.
- `dlls/cbase.cpp`'s single `dynamic_cast<CBasePlayerItem *>` is incompatible
  with the mandatory `-fno-rtti`. Replaced with the SDK's own downcast idiom, a
  `MyItemPointer()` virtual next to the existing `MyMonsterPointer()`, added at
  the *end* of the virtual list so no existing vtable slot moves.
- The rest are the familiar ones: `__PPU__` branches in the private
  `public/build.h` snapshot (dead in this SDK -- nothing includes it, so the
  code guards use `__PPU__` directly), in `public/tier0/platform.h`'s
  DLL_EXPORT block and in `game_shared/counter.h` (`<linux/limits.h>` ->
  `<limits.h>`); the dlopen half of `public/interface.{h,cpp}` compiled out;
  `creat()` -> `open(O_CREAT|O_WRONLY|O_TRUNC)` in `game_shared/bot/nav_file.cpp`;
  `Plat_IsInDebugSession` short-circuited (no `/proc`, no `getppid`); and a
  checked-in `version/appversion.h`, which upstream generates by shelling out to
  git.

**Counter-Strike goal ladder.** CS-1 (flavor identity), CS-2 (cs16-client as
`client`), CS-3 (mainui_cs as `menu`) are hardware-validated. CS-4 (ReGameDLL as
`server`) **runs on hardware as of 2026-08-05**: CS game rules initialize, the
map spawns, `4 player server started`, and the client also connects to a real
online Xash CS server (`BUILD 4068 SERVER`, modern protocol, custom content
downloaded successfully). It is not playable yet, for one reason:

- **CS-5, the next goal: the client-side memory ceiling.** Map load ends in
  `_Mem_Alloc: out of memory (alloc size 2.22 Mb, pool "FileSystem Pool",
  filesystem/io.c:51)` -> `Host_Error` -> back to the main menu (a clean
  shutdown, not a crash). It fires in `CL_PrecacheResources`, so local listen
  servers and remote servers hit it identically -- one bug, not two. Measured
  on hardware: boot 144.48 Mb largest contiguous -> `CL_PrecacheResources
  enter` 76.85 Mb -> `before VOX preload` 66.45 Mb. Note that roughly half the
  budget is already spent before map assets begin loading, which may matter
  more than any single consumer. First thing to measure, not to assume:
  `VOX_PreloadDeferred` makes 1347 HL1 vox words resident (`sound/vox/`,
  hgrunt, barney, scientist) that Counter-Strike never plays -- it uses radio
  wavs -- but goal 18 moved that preload deliberately, so do not disable it
  without the number. `PS3_ProbeMemory` calls are already staged in
  `engine/client/sound/s_load.c` and `engine/client/cl_main.c`.
- **CS-6, bots.** Needs two things together: byte-swapping the `.nav` reader
  (35 raw binary I/O sites in `regamedll/game_shared/bot/nav_file.cpp`, 3 more
  in `nav_area.cpp`; PC-authored `.nav` is little-endian, so `0xFEEDFACE` reads
  back as `0xCEFAEDFE`), and shipping `BotProfile.db`/`BotChatter.db`, which the
  staged `cstrike/` tree does not have. ReGameDLL contains **no** byte-swapping
  and **no** bitfields anywhere -- this is almost certainly its first
  big-endian build, so treat every raw binary path as suspect. A missing `.nav`
  logs *file not found*; a byte-swapped one would log *invalid format*, which is
  how to tell those two apart.

**Temporary diagnostics currently in the tree**, to be removed when CS-5 closes:
`[cs4]` `CONSOLE_ECHO` breadcrumbs in `regamedll/dlls/bot/cs_bot_manager.cpp`,
`regamedll/dlls/multiplay_gamerules.cpp`, `regamedll/dlls/world.cpp` and
`regamedll/public/FileSystem.cpp`, plus the four `PS3_ProbeMemory` calls named
above.

`3rdparty/mainui_cs/` is Velaron/mainui_cpp at `ba8802c` -- the fork
cs16-client pins as its own `3rdparty/mainui_cpp` submodule, **a different
repository from FWGS's mainui_cpp**, not a branch of it. It is what implements
`IGameMenuExports` / `GameMenuExports001` and ships the `menus/client/` windows
(`BuyMenu`, `JoinGame`, `JoinClass`) that `CGameMenuExports::ShowVGUIMenu`
switches on. Three things about it:

- **`exports.txt` needs a third line, `CreateInterface`.** That is the entire
  mechanism. `cs16-client/cl_dll/cdll_int.cpp` asks
  `gMobileAPI.pfnGetNativeObject("MenuFactory")`, which reaches
  `UI_GetMenuFactory` (`engine/client/dll_int/cl_gameui.c`) and does
  `COM_GetProcAddress( gameui.hInstance, "CreateInterface" )`. `xshlib.py`'s
  `objcopy -G lib_menu_exports` localizes every name not in that file, so
  without the line the lookup returns NULL, `g_pMenu` stays NULL, and the log
  reads `Error: native object "MenuFactory" is unavailable`. Every `g_pMenu`
  call site null-checks, so that is a missing buy menu, not a crash -- which is
  exactly the state the flavor was in before this landed. Same fix
  `filesystem/exports.txt` needed for its own `CreateInterface`.
- **Both this module and the client compile `cs16-client/common/interface.cpp`,
  and that is correct.** The registry it implements (`s_pInterfaceRegs`,
  `CreateInterface`, `Sys_GetFactoryThis`) is file-scope, and xshlib localizes
  each module's symbols, so each ends up with a private one: the client's holds
  `GameClientExports001` and hands its own factory to `g_pMenu->Initialize`
  (which is how the menu resolves `g_pClient` back); the menu's holds
  `GameMenuExports001` and its `CreateInterface` is the single globally-exported
  name. `cs16-client/cl_dll/exports.txt` deliberately does not list
  `CreateInterface`, so there is no duplicate-global collision. Do not "fix"
  this by keeping the registries global -- localization is load-bearing.
- Compiling the same `interface.cpp` from two taskgen paths is the goal-10
  `idx` collision by construction, so the module passes
  `idx = bld.get_taskgen_count()`. Verified in `build_cs/`:
  `interface.cpp.15.o` and `interface.cpp.19.o`.

Its own upstream `wscript` is FWGS's unmodified one and cannot build the fork
(no `interface.cpp`, no cs16-client include paths -- upstream only builds it via
cs16-client's CMake), so `3rdparty/mainui_cs/wscript` is
`3rdparty/mainui/wscript` plus those, plus `menus/client/*.cpp` in the glob.
`miniutl` is vendored into the tree rather than shared with `3rdparty/mainui`:
the fork's `font/BaseFontBackend.cpp` includes it as `"miniutl/utlbuffer.h"`,
which only resolves if a `miniutl/` sits next to it, and putting `../mainui` on
the include path to satisfy that would silently shadow any missing fork header
with the stock tree's copy. The two pins are the same commit (FWGS/MiniUTL
`048a416`, verified byte-identical), so the duplication costs nothing but disk.
The one vendored-tree patch is the usual `__PPU__` branch in its private
`sdk_includes/public/build.h` snapshot; no `<memory.h>`, no `_inline`, no
dlopen half to compile out, unlike `cs16-client/`.

Four things about that client are load-bearing and were each found the hard way:

- **Do not add `F` to `cs16-client/cl_dll/exports.txt`.** `cl_game.c` prefers a
  `GetClientAPI`/`F` entry point over the named export table, and `F`'s
  `cldll_func_t` has no slots for the FWGS extensions -- `IN_ClientMoveEvent` /
  `IN_ClientLookEvent` would come back NULL and the PS3 pad would lose look and
  move. cs16-client defines `F`; the exports list deliberately omits it.
- **`-fno-rtti` is required.** xshlib localizes every symbol in the module bar
  `lib_client_exports`, and localizing a COMDAT symbol makes ld discard the
  group while references survive (`typeinfo for IBaseInterface ... defined in
  discarded section`). Keeping them global instead would re-expose the goal-10
  vtable/ODR collapse, since `CBasePlayerWeapon`'s typeinfo exists in both this
  client and the HL server module.
- **`-mminimal-toc`**, for the same 64KB ELFv1 `.toc` reason as the Opposing
  Force server: this client is ~140 translation units against base HL1's 41.
- **`-fno-strict-aliasing`**, because the tree type-puns floats through int
  lvalues in the GoldSrc idiom (`Q_rsqrt`, `IS_NAN`, the shared-weapon RNG
  seed) and GCC may miscompile that at -O2.

Patches inside the vendored tree, all documented in place: a `__PPU__` branch in
its private `public/build.h` (same snapshot bug as mainui's and hlsdk's), the
dlopen half of `common/interface.{h,cpp}` compiled out under `XASH_PS3`,
`_inline` -> `static inline` in the never-before-compiled big-endian arm of
`common/xash3d_types.h`, `<memory.h>` -> `<string.h>` in six files, and a
wire-driven `snprintf` truncation in `cl_dll/health.cpp`.

A flavor is keyed by the mod's real **gamedir**, which is not always the name
of the game: Opposing Force ships as `gearbox`, so that -- not `opfor` -- is
the `--gamedir` value, the `PS3_FLAVORS` key and the `exports_<gamedir>.txt`
suffix. The define it selects is named for the game (`OPFOR`).

Each flavor builds into **its own directory**, the fourth field of its
`PS3_FLAVORS` row: the stock build in `build/`, every mod in its own `build_*`.
They compile the same sources with different `-D` flags, so a shared build dir
would hand one flavor the other's objects; configure fails if two flavors ever
claim the same directory. `out` is derived from `--gamedir` at the top of the
root `wscript` -- waf reads `out` when it loads the module, before options are
parsed, so that code reads `sys.argv` directly. An explicit `-o` still wins.
Both flavors share one waf lockfile, so a bare `./waf build` builds whichever
you configured last; the configure line prints the flavor and its directory.

`--gamedir` sets the *mod*, NOT `XASH_GAMEDIR`. That macro is the engine
**basedir**, and `filesystem/searchpath.c` only adds the base game hierarchy
when basedir differs from the gamefolder -- so the flavor block pins
`XASH_GAMEDIR` to `valve` and passes the mod through `-game`, injected into
argv by `engine/common/launcher.c` (the XMB launches EBOOT.BIN with no
arguments). Folding both into `XASH_GAMEDIR` drops `valve/` out of the search
path entirely and takes every shared asset with it; the visible symptom is raw
`GameUI_*` tokens in the menu. `fallback_dir` in liblist.gam is not involved
and does not need editing. All flavors resolve their filesystem root to the
same shared `/dev_hdd0/data/xash3dfwgs`, so `valve/` is never duplicated.

**Per-flavor game code.** There is one vendored `hlsdk-portable` tree, not one
per mod, so a mod's divergence from base HL1 lives inline in that tree behind
its own `#ifdef` and the valve build must stay semantically identical to
upstream `FWGS/hlsdk-portable` master. The gamedir -> define mapping is
`PS3_GAME_DEFINES` at the top of the root `wscript` (`bshift` -> `BSHIFT`,
`gearbox` -> `OPFOR`); it lands in `conf.env.GAME_DEFINES`, which
`hlsdk-portable/dlls/wscript` and `hlsdk-portable/cl_dll/wscript` append to
their own `defines` lists.

Inline `#ifdef` is the right tool only when a mod *edits* base HL1 code, as
Blue Shift does. Opposing Force is mostly **new** code, and it lives in its own
`dlls/gearbox/` and `cl_dll/gearbox/` subdirectories -- `gearbox` being the
directory the mod ships under. Both wscripts build their source list from a
recursive `**/*.cpp` glob, so those directories are added to `excluded_files`
for every flavor except `gearbox`; `#ifdef`ing the file bodies instead would
still compile 57 translation units of gearbox entities into the valve and
bshift links. The same condition adds `gearbox` to the server's include path,
and adds `../dlls/gearbox` plus Opposing Force's ten predicted weapon sources
to the client (`CLIENT_WEAPONS` needs each weapon's server-side implementation
compiled into the client, exactly like the base HL1 list above it).

Entity symbols a mod adds go in `dlls/exports_<gamedir>.txt`, **not** in the
shared `exports.txt`: `xshlib.py` emits every name there as an
`extern void x(void);` plus a table entry, so a mod-only entity in the shared
file would break the valve link on a symbol its own `#ifdef`'d-out sources
never define. `apply_xshlib` appends `exports_${PS3_GAME}.txt` when that file
exists. Blue Shift's five: `env_warpball`, `item_armorvest`, `item_helmet`,
`monster_rosenberg`, `trigger_playerfreeze` (251 exports for valve, 256 for
bshift). Opposing Force adds **95** in `dlls/exports_gearbox.txt`, for 346.
`trigger_playerfreeze` is in both mods' lists and that is fine -- bshift's
implementation is gated inside `dlls/triggers.cpp`, Opposing Force's lives in
`dlls/gearbox/gearbox_triggers.cpp`, and no build ever defines both flavors.

The Blue Shift game code itself is the diff of upstream branch `bshift`
against `master` -- 9 files: `items.cpp` (armor vest + helmet pickups),
`player.cpp` (impulse 101 gives those instead of a battery), `weapons.cpp`
(precache both), `scientist.cpp` (`monster_rosenberg` -- same class as
`CScientist`, every difference selected at runtime off the classname:
double health, RO_* sentence groups, nine pain lines, never provoked by the
player, never flees), `genericmonster.cpp` (`SF_HEAD_CONTROLLER` spawnflag 8
+ head tracking), `effects.cpp` (`CLightning::LightningCreate` and
`env_warpball`), `triggers.cpp` (`trigger_playerfreeze`), `talkmonster.cpp`
(comment only) and `cl_dll/hud.h` (blue `RGB_YELLOWISH`). Diff against
upstream master, never against the vendored tree -- the latter carries PS3
patches that pollute the comparison.

Opposing Force's own edits to base HL1 land the same way, but there are far
more of them, so they are being applied in stages. **Stage 1 (done): the
shared headers.** Sixteen of them carry `#ifdef OPFOR` blocks now --
`weapons.h` (ten weapon classes, their ammo/clip/weight/give constants, three
new player bullet types and three new monster ones), `cbase.h` (three Race X /
military-ally `CLASS_*` values, `GrappleTarget`, the `PreRemoval`/`OnRemove`/
`PostRemoval` hooks, `CreateNoSpawn`, `SizeForGrapple`, four ammo counters,
and a `virtual` on `CBaseButton::ButtonActivate`), `player.h`, `skill.h` (a
second, Op4-only half of `skilldata_t`), `talkmonster.h` (Op4 reparents
`CTalkMonster` onto `CSquadMonster` and raises `TLK_CFRIENDS` 3 -> 6),
`basemonster.h` (glowshell), `gamerules.h`, `util.h`, `cdll_dll.h`,
`decals.h`, `effects.h`, `explode.h`, `monsters.h`, `schedule.h`,
`scripted.h`, `pm_materials.h`, plus `cl_dll/hud.h` (`CHudNightvision` and a
three-way `RGB_YELLOWISH`: valve orange / bshift blue / Op4 green). That
cleared all 1,833 compile errors the gearbox flavor started with; the
remaining work is stage 2.

Three rules learned doing it, worth keeping:

- **Gate only what the mod adds, not what it happens to rename.** The opfor
  branch also renames `CBasePlayer::m_flSndRoomtype` to `m_SndRoomtype` and
  repoints `SOUND_FLASHLIGHT_OFF`. Neither is needed by any gearbox source,
  and pulling them in would break the shared `player.cpp` that still uses the
  old names. Skipped deliberately. (Stage 2 revisited the first half of that:
  Op4's roomtype rework is not a pure rename -- it changes the field to `int`,
  save-restores it, adds `m_ClientSndRoomtype` and moves the `SVC_ROOMTYPE`
  send from `CEnvSound::Think` into `CBasePlayer::UpdateClientData` -- so
  `player.h` now carries all three names behind `#ifdef OPFOR`. The
  `SOUND_FLASHLIGHT_OFF` repoint is still skipped, still unused.)
- **Enum tails take a leading comma, not a trailing one.** `Bullet` and
  `decal_e` end with `#ifdef OPFOR` blocks written as `,NEW_VALUE` so the
  non-Op4 build has no dangling comma to `-Wpedantic` about.
- **Prove the other flavors are untouched mechanically.** A ~40-line
  mini-unifdef that resolves only `#ifdef OPFOR` blocks and diffs the result
  against `HEAD` reduces the whole change to "identical with OPFOR undefined"
  for every shared file. Same technique the Blue Shift work used. It does not
  understand `#elif defined( OPFOR )` inside a foreign `#ifdef` chain
  (`cl_dll/hud.h`'s three-way colour), so check that one by eye.

**Stage 2 (done, VALIDATED on real hardware 2026-08-04): the 54 shared `.cpp`
files plus `cl_dll/ev_hldm.h`.** Confirmed on console: boots to the Op4 menu,
loads the campaign, scripted sequences run, HUD and nightvision correct,
events correct, all ten Op4 weapons work, save/reload works, and level
transitions work -- the last two being the ones at risk, since the Op4 weapons
add save data and `m_SndRoomtype` changed type. Opposing Force is complete;
there is no stage 3. These files carry the source half of what stage 1
declared --
`CTalkMonster::StartMonster`, the CTF message globals
(`gmsgCTFMsgs`/`gmsgFlagCarrier`/`gmsgRuneStatus`/`gmsgFlagStatus`) in
`client.cpp`, `env_spritetrain` in `plats.cpp`, and the
`IMPLEMENT_SAVERESTORE` block in `weapons.cpp` for the seven Op4 weapons that
have save data (missing that is what "undefined reference to `vtable for
CDisplacer`" means -- `Save` is the vtable's key function). Biggest single
pieces are `cl_dll/ev_hldm.cpp` (+604), `dlls/game.cpp` (+535),
`dlls/plats.cpp` (+302) and `dlls/player.cpp` (+286/-31). All three flavors
build clean afterwards; the valve link produced bit-identical objects, so waf
skipped its downstream package tasks entirely -- a free confirmation that the
untouched flavors really are untouched.

The gating was generated, not typed: a helper diffs upstream
`merge-base(master, opforfixed)` against `opforfixed`, replays that patch onto
the *vendored* file (so this port's own local edits survive), and wraps every
resulting difference in `#ifdef OPFOR` / `#else` / `#endif`. Each file is then
proved by resolving OPFOR both ways and comparing against the two inputs. Two
files need a hunk skipped because the change is already applied locally
(`dlls/genericmonster.cpp`, whose class carries a `#ifdef BSHIFT` block, and
`cl_dll/hl/hl_weapons.cpp`, whose `HUD_PrepEntity` calls carry `PS3_DIAG`
markers); their few remaining lines were added by hand.

**Two traps this stage produced, both worth remembering.**

- **A generated `#endif` may not land inside a `/* */` comment.** Comments are
  removed in translation phase 3, `#if` is evaluated in phase 4, so a
  directive inside a comment silently disappears and takes the rest of the
  comment's contents with it. Op4 comments out `func_tank.cpp`'s
  `SF_TANK_*`/`TANKBULLET` block rather than deleting it, and a naive
  line-diff gate put `#endif // OPFOR` between the `/*` and the `*/` -- the
  OPFOR build was fine and the *valve* build lost seven `#define`s. Both the
  generator and a repo-wide audit now track comment state and refuse a gate
  boundary inside one. Note the mini-unifdef proof does **not** catch this: it
  is textual and knows nothing about comments.
- **The ODR trap stage 1 warned about is closed.** Goal 1 vendored the 13
  headers Op4 extracts from shared `.cpp` files (`barney.h`, `hgrunt.h`,
  `scientist.h`, `zombie.h`, `xen.h`, `func_tank.h`, `triggers.h`, `apache.h`,
  `bullsquid.h`, `genericmonster.h`, `gman.h`, `headcrab.h`, `osprey.h`) while
  those `.cpp` still defined the same classes inline. Each of those files now
  reads `#ifdef OPFOR` include-the-header `#else` inline-class `#endif`, so
  only one definition is ever live and the valve/bshift text is unchanged.
  This is deliberately *not* "adopt the header everywhere": several extracted
  headers add `virtual` to methods that were non-virtual in master, which
  would change vtable layout for flavors that did not ask for it.

**Counter-Strike does not fit this model, and must not be forced into it.**
Every mechanism above -- one vendored `hlsdk-portable` tree, per-flavor
`#ifdef`s, `exports_<gamedir>.txt` -- assumes the mod is a *variant of the
HL1 SDK*. CS 1.6 is not. Its client is the separate `cs16-client` SDK
(`E:\Users\Matteo\Desktop\HL1\PS3\cs16-client`, ~140 client translation units
with its own private `common`/`public`/`pm_shared`/`game_shared`), its server
is `ReGameDLL_CS`, and its menu is Velaron's `mainui_cpp` fork. So the CS
flavor selects a *different source tree* per waf name rather than gating the
hlsdk one: a pair of `SUBDIRS` rows per name, gated on `PS3_GAME == 'cstrike'`,
so exactly one tree supplies each (`xshlib.py` matches the waf `name=`, never
`target=`). No `PS3_GAME_SDK` table was needed -- the pairs read fine inline,
and the earlier plan for one is superseded. All three have landed.

Every CS tree carries a private `build.h` snapshot with `__SWITCH__`/`__vita__`
branches and **no `__PPU__`** -- `cs16-client/public/build.h`,
`3rdparty/mainui_cs/sdk_includes/public/build.h` and `regamedll/public/build.h`,
all patched the same way as `hlsdk-portable`'s and `3rdparty/mainui`'s copies.
That prediction held three times for three; assume the next vendored tree has
one too. ReGameDLL adds a wrinkle worth remembering: its snapshot is *dead*
(nothing in that SDK includes it), so its own platform guards had to key off
`__PPU__` directly.

**The valve EBOOT is not byte-reproducible**, so "rebuilt byte-identical (md5)"
is not a usable regression test -- relinking with zero source changes yields a
different md5 at an identical byte size. Compare the size, and confirm via
`git status` that no file the valve build compiles was touched.

**PPC64 TOC overflow is a real ceiling here.** Adding ~57 gearbox translation
units pushed the final link past the 64KB `.toc` an ELFv1 `R_PPC64_TOC16_DS`
can address (`relocation truncated to fit`). ld's own multi-TOC splitting
cannot help, because `xshlib.py` merges each statically-linked module into one
relocatable object first -- the linker sees a single input file and therefore
a single TOC group. `hlsdk-portable/dlls/wscript` adds `-mminimal-toc` for
this flavor only, which gives each translation unit its own constant pool
behind one TOC entry, at the cost of one extra indirection per reference.
Scoped to gearbox on purpose: valve and bshift still fit and their builds are
hardware-validated, so there is no reason to change their codegen. If a future
flavor or a growing valve build hits the same error, widen that condition
rather than inventing something new.

Deploy (fast iteration): FTP `<builddir>/engine/pkg/USRDIR/EBOOT.BIN` to
`/dev_hdd0/game/<TITLE_ID>/USRDIR/` via webMAN/multiMAN, then relaunch --
`build/` -> `XASH10000` for the stock build, `build_bs/` -> `XASHBS000` for
Blue Shift, `build_opfor/` -> `XASHOF000` for Opposing Force,
`build_ricochet/` -> `XASHRC000` for Ricochet, `build_cs/` -> `XASHCS000` for
Counter-Strike, per the flavor table above. Deploy (clean install): install the
`.pkg` from USB via XMB -- required the first time a flavor is deployed, since
a `TITLE_ID` with no existing install has no `USRDIR/` to FTP into. Each
flavor installs to its own `TITLE_ID` and gets its own XMB entry, so flavors
coexist without overwriting each other.

Debug: UDP log sink -- run `nc -ul 18194` on the dev PC; wire the sending
side up first thing in `PS3_Init()` (`sys_ps3.c`), per goal-stack item 1.
There is no GDB stub for retail homebrew.

## 8. Repo layout after the 2026-08-04 cleanup

The tree was reduced to exactly what the PS3 build compiles, links and
packages. Verified by rebuilding from scratch afterwards and confirming the
translation-unit count is byte-for-byte unchanged (616 TUs, same per-directory
breakdown as before the cleanup) -- nothing that reaches the binary was
touched. What went, and why:

- **Other platforms.** `engine/platform/{android,dos,ios,irix,linux,nswitch,
  psvita,sdl1,sdl2,sdl3,stub,win32}` -- `engine/wscript` only globs
  `platform/$DEST_OS/*.c` plus `platform/posix`, so none were ever compiled.
  `android/` (Android Studio project), `game_launch/` (`env.LAUNCHER` is
  forced off for ps3), `utils/`, `.builds/`, `.github/workflows/`,
  `scripts/{cirrus,flatpak,gha,ios,sailfish}` went with them.
- **Other renderers.** `ref/null`, `ref/gl/vgl_shim` (PSVita's vitaGL shim),
  and the `ref_gles1`/`ref_gles2`/`ref_gl4es`/`ref_gles3compat` targets in
  `ref/gl/wscript`, plus their backends `3rdparty/{nanogl,gl-wes-v2,gl4es}`.
  `ref_soft` and `ref_gl` (over `3rdparty/ps3gl`) both stay -- see section 7.
- **Unbuilt 3rdparty subprojects.** `extras` (builds `extras.pk3`, which
  `scripts/waifulib/ps3.py` never puts in the PKG -- and shipping its fonts
  is a known regression, see the goal-18 notes), `mbedtls`, `libbacktrace`,
  `libogg`, `vorbis` (the last two resolve to ps3dev portlibs via pkg-config,
  so the bundled copies never configured), `maintui`, `vgui_support`,
  `yy-thunks`.
- **Vendored-library cruft.** opus/opusfile/bzip2 kept only the sources their
  wscripts actually list, plus headers and licenses; their autotools/CMake/
  meson/MSVC projects, docs, tests, demos and non-PPC SIMD trees
  (`celt/{arm,x86,mips}`, `silk/{arm,x86,mips,fixed}`) are gone.
- **Excluded sources.** The `.cpp` files `hlsdk-portable/{dlls,cl_dll}/wscript`
  name in their `excl=` lists (VGUI, goldsource input, debug stubs) were
  deleted rather than left to confuse; all headers were kept.
- **Test-only trees.** `public/tests`, `filesystem/tests`,
  `3rdparty/*/tests` -- `bld.env.TESTS` is never set for this target.
- **Bring-up tools.** `tools/ps3_bringup` and `tools/ps3_video` (goals 1 and 4,
  both closed and hardware-validated long ago) and `tools/gen_reslist`. The
  goal entries above still describe them; recover from git history or the
  disk backup if a future RSX experiment wants the standalone harness back.

Kept deliberately even though the build does not consume them: `Documentation/`,
`icons/` (only `icons/hl1/ICON0.PNG` is packaged, the rest are for future mod
flavors), `packaging/ps3/README.md` (cross-referenced from
`3rdparty/mainui/wscript`), `CONTRIBUTING.md`, `SECURITY.md`, `.clang-format`,
`.editorconfig`, and `hlsdk-portable/external/openbsd/strlc{py,at}.c` (the
wscripts still reference them behind the `HAVE_STRLCPY`/`HAVE_STRLCAT` probes).
