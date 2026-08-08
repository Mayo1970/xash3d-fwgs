# xashPS3

PlayStation 3 homebrew port of [Xash3D-FWGS](https://github.com/FWGS/xash3d-fwgs)
(a GoldSrc-compatible engine), built against the open-source PSL1GHT /
ps3toolchain homebrew SDK. Real PS3 hardware (CFW/HEN) only.

Currently ships **Half-Life 1**, **Opposing Force** and **Blue Shift**.

See [AGENTS.md](AGENTS.md) for the porting bible: goal stack, hardware
validation protocol, platform conventions, and known blockers.

## Building

Requires a `ps3dev/ps3dev:latest` Docker image (or a local PSL1GHT/
ps3toolchain install) with `PS3DEV` (or `PSL1GHT`) pointing at it.

Half-Life 1 (`valve`):

```
./waf configure --ps3 --static-linking=filesystem_stdio,ref_soft,ref_gl,menu,server,client
./waf build
```

Opposing Force (`gearbox`):

```
./waf configure --ps3 --gamedir=gearbox --static-linking=filesystem_stdio,ref_soft,ref_gl,menu,server,client
./waf build
```

Blue Shift (`bshift`):

```
./waf configure --ps3 --gamedir=bshift --static-linking=filesystem_stdio,ref_soft,ref_gl,menu,server,client
./waf build
```

Each flavor builds into its own directory and gets its own `TITLE_ID`, so
they can be installed side by side without overwriting each other:

| Flavor | `--gamedir` | Build dir | Title ID |
|---|---|---|---|
| Half-Life 1 | (none) | `build/` | `XASH10000` |
| Opposing Force | `gearbox` | `build_opfor/` | `XASHOF000` |
| Blue Shift | `bshift` | `build_bs/` | `XASHBS000` |

Each build produces `EBOOT.BIN`, `PARAM.SFO` and an `EBOOT.pkg` staged under
`<builddir>/engine/pkg/`.

## Installing on PS3

**First install of a flavor** (no existing `TITLE_ID` on the console yet):
copy `EBOOT.pkg` to a USB stick and install it from the XMB Package Manager.

**Fast iteration after that**: FTP `<builddir>/engine/pkg/USRDIR/EBOOT.BIN`
to `/dev_hdd0/game/<TITLE_ID>/USRDIR/` on the console (e.g. via webMAN or
multiMAN's built-in FTP server), overwriting the existing file, then relaunch
the game from the XMB -- no full package reinstall needed.

Game assets (the `valve`/`gearbox`/`bshift` content trees) are not bundled in
the package -- they must be present under
`/dev_hdd0/data/xash3dfwgs/<gamedir>/` before launching (FTP them over once
per flavor).

### Installing the expansions (Opposing Force, Blue Shift)

Opposing Force and Blue Shift are each a **separate flavor with their own
`TITLE_ID`** (see the table above), not a switch inside the base install:

1. Build (or download) that flavor's `EBOOT.pkg` and install it from the XMB
   Package Manager like any other title -- it lands under its own
   `/dev_hdd0/game/<TITLE_ID>/`, side by side with Half-Life 1 and any other
   flavor already installed.
2. FTP that flavor's asset tree (`gearbox/` for Opposing Force, `bshift/` for
   Blue Shift) to `/dev_hdd0/data/xash3dfwgs/<gamedir>/` on the console.
3. Launch the flavor's own icon from the XMB. Each flavor is independent, so
   installing one does not touch or overwrite another.

**Before first launch of a flavor**, delete any `config.cfg`, `userconfig.cfg`
and `autoexec.cfg` inside that flavor's `cfg/` folder (and the gamedir root)
-- i.e. under `valve/`, `gearbox/` and `bshift/` inside
`/dev_hdd0/data/xash3dfwgs/` -- if you copied them over from a PC install or
from a previous xashPS3 version. PC-generated configs carry settings/binds
that don't match the PS3 build (e.g. keyboard binds, PC video options), and
stale configs from an older xashPS3 build can carry cvars/binds the current
build no longer expects. Either can cause broken input or a black/incorrect
video mode. Let the engine regenerate fresh configs on first run instead.

## Using the in-game console

The PS3 has no physical keyboard, so the console is driven through the
controller and the system's on-screen keyboard (OSK):

- **Triangle** opens the on-screen keyboard to type a console command.
- **Cross** sends the typed command to the console.

## Known issues

- **Flashlight/torch lighting is visually wrong.** Toggling the flashlight
  does change the scene (dynamic lightmap updates and moves with the view),
  but the final composited lighting is not correctly localized the way it is
  on PC -- it doesn't produce the expected illuminated cone around the aim
  point. Root cause not yet found; see goal 16 in [AGENTS.md](AGENTS.md) for
  the full hardware-validated investigation and dead ends.

## AI disclosure

Significant portions of this port -- platform bring-up, build-system
integration, renderer/audio/input backends, and bug fixes across the engine
and vendored game code -- were developed with the assistance of Claude
(Anthropic), used as a coding tool under human direction and review. All
changes were validated on real PS3 hardware.

## Credits

- [Xash3D-FWGS](https://github.com/FWGS/xash3d-fwgs) team -- the engine this
  port is built on.
- [Valve](https://www.valvesoftware.com/) -- Half-Life, Opposing Force and
  Blue Shift, and the original GoldSrc engine/SDK.
- [Gearbox Software](https://www.gearboxsoftware.com/) -- developed Opposing
  Force and Blue Shift.
- [PS3Dev](https://github.com/ps3dev) -- PSL1GHT and ps3toolchain, the
  open-source homebrew SDK this port is built against.
- [FWGS/library-suffix](https://github.com/FWGS/library-suffix) -- platform
  detection used by the build system.

## Upstream projects (read-only reference, not edited here)

| Project | Role | Link |
|---|---|---|
| xash3d-fwgs | Vendored engine source | https://github.com/FWGS/xash3d-fwgs |
| library-suffix | Platform detection (`build.h`) | https://github.com/FWGS/library-suffix |
| PSL1GHT | Homebrew SDK (rsx/io/audio/sysutil/net) | https://github.com/ps3dev/PSL1GHT |
| ps3toolchain | PPU/SPU cross-compiler | https://github.com/ps3dev/ps3toolchain |
