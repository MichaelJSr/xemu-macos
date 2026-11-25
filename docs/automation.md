# Automation interface

What test harnesses (the Azurik Modkit among them) program against: headless
modes, QMP commands and events, gdbstub extensions, `XEMU_*` variables and
stderr lines. Everything works windowed or headless unless noted.
`cmd {args}` stands for `{"execute": "cmd", "arguments": {args}}`;
[qapi/xemu.json](../qapi/xemu.json) is the full QMP schema.

## Contents

1. [Headless mode (automation)](#headless-mode-automation)
2. [Launch rules](#launch-rules)
3. [Input](#input)
4. [Frames and deterministic mode](#frames-and-deterministic-mode)
5. [Checkpoints](#checkpoints)
6. [Hooks and run-until](#hooks-and-run-until)
7. [RAM digests, dumps and watch logs](#ram-digests-dumps-and-watch-logs)
8. [Block, voice and audio logs](#block-voice-and-audio-logs)
9. [Trace recording](#trace-recording)
10. [Savestate loading](#savestate-loading)
11. [gdbstub](#gdbstub)
12. [Guest checks and per-run settings](#guest-checks-and-per-run-settings)
13. [Profiling counters](#profiling-counters)
14. [Guest RAM above 128 MiB](#guest-ram-above-128-mib)
15. [PAL 50 Hz](#pal-50-hz)
16. [QMP events and failures](#qmp-events-and-failures)

## Headless mode (automation)

| `XEMU_HEADLESS` | Effect |
|---|---|
| unset or empty | Normal windowed xemu |
| `1` | Offscreen: hidden window, no Dock icon, never takes focus, no UI, no host audio. The guest still renders, so `screendump` returns real frames, at render resolution (no MetalFX upscaling or frame interpolation) |
| `null` | As `1`, plus the null renderer: no GPU work. Game logic, vblank timing, input, gdbstub and QMP work as usual; `screendump` returns the VGA scanout (usually blank). The configured renderer is not saved over |

stderr says `xemu: headless mode: offscreen` (or `null`) at startup. Config,
portable mode, savestates, HDD, EEPROM and command-line options behave as
windowed. In-app notifications go to stderr as `xemu: notification: <text>`.

### Screenshots

`screendump {"filename": "/tmp/f.png", "format": "png"}` (default `ppm`; QMP
only, not HMP) returns once the file is written. It holds the guest's last
flip, once the GPU has finished compositing it; while no renderer frame
covers the scanout (after a load, before the guest draws) it holds the guest
RAM there, at the video mode's size. With `perf.async_pipeline_compile` on,
a normal-mode Vulkan run skips a draw whose pipeline is still compiling for
its first use, so a picture can lack it for a frame or more; icount runs
start no compile worker.

### Picture age

`query-xemu-display` only reads counters:

| Key | Meaning |
|---|---|
| `frame`, `loads` | Frame counter and successful loads now (checkpoint restores count) |
| `flips` | Guest flips so far (writes of the CRTC start address); Azurik makes two per present |
| `scanout` | Guest physical address on screen (the CRTC start) |
| `flip-frame`, `flip-loads` | Frame counter and load count at the last flip. `flip-loads` below `loads`: no flip since the last load, so the picture is the loaded state's own |
| `frame-ends` | Guest frame ends (NV097 `FLIP_STALL`) since launch, counted whether or not GPU counting runs; its change over a span of `frame` is the frame rate (0.16.3+) |
| `picture-stale` | True while the picture on screen predates the last load: no GPU draw, clear or 2D blit into the buffer on screen has counted since (a write counts once a frame begun after the load ends). Present whenever a drawing renderer runs (0.16.1+) |
| `picture-loads`, `picture-frame` | Load count and frame at the last counted write; absent before the first, and under null |

Date a picture by `picture-stale`, not `flip-loads`: a load drops the
renderer's surfaces, and a game that flips the same buffer every vblank
(Azurik) can flip a frame built from the loaded RAM, so `flip-loads` equals
`loads` while the picture is still the save's. A fresh process that
restores the same checkpoint shows the same picture. A game that draws into
the scanout before it flips shows that frame to a screendump taken in
between, and windowed frame interpolation can show a step between the last
two flips. A buffer only the CPU or the video overlay writes stays stale
until the GPU writes it, so bound any advance-until-fresh loop.

### Pictures with a null run's RAM

`XEMU_RENDER_NULL_RAM=1` (Vulkan, offscreen or windowed; stderr
`xemu: render null RAM: all`) draws every frame but leaves guest RAM as the
null renderer does: zpass reports are 0, no rendered pixels are written back
(not at `savevm` either), image blits don't run. RAM digests equal a null
run's at every frame; pictures lack what needs rendered pixels in RAM
(textures sampled from render targets the renderer can't bind, blits). Other
values than `1` and `request` are ignored, with a line saying so.

`XEMU_RENDER_NULL_RAM=request` (stderr `xemu: render null RAM: request`)
also skips draws, costing about a null run, until
`xemu-render-next {"flips": N}` runs the VM from the guest's next flip until
N whole frames are drawn and the last is on screen, then pauses. The reply
is the frame info, as `xemu-frame-advance` returns it, plus `render-flips`
(frames drawn, possibly more than N); take the `screendump` then.
`"frames"` caps the vblanks (default 120; raise it when `render-flips` falls
short, e.g. with cold shader caches). Without `flips` it only arms the draws
for the VM's next run and the pause after it (given while the VM runs:
until it is paused and started again; a load or save restarts the VM). It
fails, arming nothing, if the VM can't run (`restore-vm`, shutdown, error),
outside `request` mode, or under null. The first frame drawn lacks render
targets earlier frames would have made, so use `"flips": 2`. RAM stays the
null run's.

## Launch rules

- Give every run its own `-config_path` (a scratch `xemu.toml`) and the
  marker below. With neither `-config_path` nor `XEMU_BASE_PATH`, any
  launch, even `--version`, uses the player's `xemu.toml` and HDD and saves
  the toml at exit.
- Mute windowed runs (`[audio] volume_limit = 0`) or run headless.
- Keep `display.ui.pause_on_focus_loss` off in windowed runs: it pauses the
  guest when the window loses focus.

### Automated-run marker

xemus started by this fork's tooling carry `-name xemu-fork-auto` right
after the binary (`ps -axo command` shows it). Interactive ones never do,
and the marker turns the player's `[automation]` table off. Every launch
line in `scripts/`, the agent skills and the docs carries it or
`XEMU_BASE_PATH`.

The macOS launchers in `scripts/` also apply `nice -n 10`, the bench cache
as `XEMU_BASE_PATH`, a lease `/tmp/xemu-leases/<xemu pid>.json` and one of
`XEMU_FORK_SLOTS` (4) slots: an `flock` on `/tmp/xemu-leases/slots/<i>.lock`
through a descriptor xemu inherits, so it lasts as long as that xemu, even
if its launcher is killed. The contract is in `scripts/xemu_lease.py`:

```sh
# nice, marker, bench cache, lease; then becomes the xemu (same pid)
python3 scripts/xemu_lease.py run -- <xemu> -config_path <toml> ...
python3 scripts/xemu_lease.py    # a running demo and the live leases
```

`XEMU_LEASE_DIR` moves the leases and slots (tests only).

**Owner demo.** While `/tmp/xemu-leases/owner-demo.json` names a running
pid, the launchers and `run` start no xemu: they print
`lease: owner demo running (pid N; ...): no new xemu until it ends; waiting`
once a minute, or exit 75 with `XEMU_OWNER_DEMO_WAIT=0`. The harness writes
it for each owner sitting, atomically (temp file in the same folder, then
rename): a JSON object with an integer `"pid"` that lives as long as the
sitting, and optional `"who"`, `"purpose"` and `"expected_end"` strings. A
dead pid or an unparsable file counts as no demo. Under `XEMU_LEASE_DIR` the
moved folder's `owner-demo.json` counts too.

**Inherited variables.** The launchers (and the Windows bench) drop these
from the environment they pass on, saying
`lease: not passing the inherited ...`: `XEMU_RAM_SHM`, `XEMU_INPUT_PIPE`,
`XEMU_INPUT_RECORD`, `XEMU_INPUT_RECORD_PORT`, `XEMU_TRACE_RECORD`,
`XEMU_TRACE_REGIONS`, `XEMU_BLOCK_LOG`, `XEMU_VOICE_LOG`, `XEMU_AUDIO_WAV`,
`XEMU_ICOUNT_LIVE_PADS`, `XEMU_VK_DRIVER`, `XEMU_QMP_WATCH`,
`XEMU_LANGUAGE`, and the opt-ins that change a run's RAM, cycles or QMP
events: `XEMU_RENDER_NULL_RAM`, `XEMU_LOADVM_SHRINK_RAM`,
`XEMU_AGP_FULL_DECODE`, `XEMU_CSTATUS_FULL_RAM`, `XEMU_SOCKET_BACKLOG`,
`XEMU_STRICT_CPUID`, `XEMU_GUEST_BUGCHECK`. Set one per run instead (`VAR=V`
arguments, an arm's `K=V`). Normal-mode switches (`XEMU_VCPU_QOS` and the
rest) pass through, since an A/B sets them per arm.

### Readiness and data folder

stderr says `xemu: ready` once the machine and pads exist (about 30 ms after
launch in headless null mode; `loadvm` works from then on, no fixed sleep)
and `xemu: state loaded` after every `loadvm` or incoming migration; see
[Events](#events).

`XEMU_BASE_PATH=<dir>` moves the data folder (created if missing; ahead of
portable mode): `spirv_cache_v*/`, `pipeline_cache.bin`, the
`vk_prewarm-*.bin` pipeline list, and `xemu.toml` and `eeprom.bin` unless
`-config_path` or `eeprom_path` names them. Unset, it is the portable folder
or the SDL pref dir (`~/Library/Application Support/xemu/xemu/` on macOS);
`-config_path` never moves it. stderr:
`xemu_settings_get_base_path: base path: <dir>/`.

### `[automation]` in the player's `xemu.toml`

```toml
[automation]
ram_shm = "azurik-live"                        # as XEMU_RAM_SHM
qmp = "tcp:127.0.0.1:4444,server=on,wait=off"  # what -qmp takes
```

Endpoints for a tool that reads the player's running game, with no launch
arguments. The table applies only when `xemu.toml` is the SDL pref dir's (no
`-config_path`, no `XEMU_BASE_PATH`, not portable mode) and the name is not
`xemu-fork-auto`, so harnesses and benches never take the player's name or
port, even from a copied toml. A non-empty `XEMU_RAM_SHM` overrides
`ram_shm`; a QMP monitor on the command line (`-qmp`, `-qmp-pretty`,
`-mon mode=control`) overrides `qmp`, whose `wait` is always off.

Each set key logs `xemu: [automation] <key> = "<value>": applied`,
`ignored (<why>)` or `skipped: <why>`. An endpoint that can't open (address
and port taken, a live listener on the unix path, a live xemu holding the
name) is skipped with a notification and xemu keeps running; a listener on
another address doesn't count on macOS (`SO_REUSEADDR`). `open` without
`-n` starts no second copy while any windowed or offscreen xemu runs,
clones included (they keep the bundle id `app.xemu.xemu`; headless `null`
runs don't count). Settings >
General > Automation endpoints (`automation.enabled`) turns the table off;
builds without the table drop it when they save `xemu.toml`.

### Sockets and monitors

- Unix socket paths (`-qmp unix:`, `-monitor unix:`, `-gdb unix:`,
  `-chardev socket,path=`, `chardev-add`) are never taken from a live
  listener: the new socket fails with
  `UNIX socket <path> is in use by a live listener: Address already in use`
  (at launch xemu exits 1). Stale sockets and plain files are replaced; a
  closing listener unlinks the path only if it is still its own socket.
- On macOS a QMP monitor serves one client and queues one more; further
  connects get `ECONNREFUSED` (busy, not gone). `XEMU_SOCKET_BACKLOG=<n>`
  (1-4096, capped by the kernel: macOS `kern.ipc.somaxconn`, 128 by default)
  queues n on every listening socket chardev (`-qmp`, `-monitor`,
  `XEMU_QMP_WATCH`, `-gdb`, a `server=on` `-serial` or `-chardev`); each is
  served, greeting first, once those ahead close, even one whose client
  already gave up (its greeting goes nowhere). Unset or out of range
  (stderr warns): 1.
- `XEMU_QMP_WATCH=<spec>` (a `-qmp` spec, e.g.
  `unix:/tmp/w.sock,server=on,wait=off`) opens a watch monitor (stderr
  `xemu: QMP watch monitor on <spec>`; exits 1 if it can't open). Other
  monitors run commands one at a time and are held while
  `xemu-frame-advance`, `xemu-run-until` or `xemu-run` wait for vblanks. The
  watch monitor answers at once, without `oob`, and only `qmp_capabilities`,
  `query-commands`, `query-version`, `query-status`, `query-xemu-frame`,
  `query-xemu-status`, `query-xemu-display`, `query-xemu-input` and
  `query-xemu-host-profile` (`query-commands` lists just these); anything
  else gets `CommandNotFound`, whose desc names them. It receives events.
  The other monitors keep their order, so `xemu-hooks-drain` never cuts
  into an advance.

### Background priority and host cost

Under `taskpolicy -b` (macOS) every thread of a headless xemu runs at
priority 4, almost only on the efficiency cores, and its CPU time bills to
background QoS: neither its user-interactive thread requests nor null
mode's app role lift it. Deterministic runs give the same RAM but go, and
answer QMP, only as fast as idle efficiency cores allow (on a loaded host a
3 s advance took up to 16 minutes): use long timeouts. Real-time runs drop
vblanks. `taskpolicy -c utility` is the middle ground (priority 20,
performance cores allowed). A headless null icount instance at 128 MiB uses
0.1% of a core while paused and one core while advancing, and takes about
165-230 MB plus about 75 MB per full checkpoint.

## Input

### Input pipe

`XEMU_INPUT_PIPE=<path>` names a FIFO (a named pipe on Windows) whose lines
drive the keyboard pad (port 2 in the fixtures) through the keyboard map,
with the window unfocused too:

| Line | Effect |
|---|---|
| `down <scancode>`, `up <scancode>` | Press or release an SDL scancode |
| `axis lx\|ly\|rx\|ry <-1..1>` | Set a stick axis; positive is right/up; `0` hands it back to the keys |
| `trigger lt\|rt <0..1>` | Set a trigger |
| `clear` | Release every key and axis |
| `@<frame> <line>`, `+<frames> <line>` | Apply the line at that frame instead of on arrival (`+0`: the next vblank) |

An untagged `clear` also drops every queued line, so nothing written before
it lands later (under icount it releases at the next vblank); `+0 clear`
keeps the queue. The host-gamepad dead zones (`input.deadzone_left`,
`input.deadzone_right`) don't touch pipe values.

### Pipe lines over QMP

`xemu-input-queue {"lines": [...]}` queues up to 262,144 lines per call
(within QEMU's 64 MiB request limit) as if written to the pipe now, after
the lines already in the pipe. `XEMU_INPUT_PIPE` need not be set: lines
drive the keyboard pad on whichever port has the `keyboard` source. Every
line is checked first: an invalid one refuses the call as
`line <n>: <why>` (n from 1) and queues nothing. Blank and `#` lines are
skipped. It returns `{accepted, skipped, queued, frame}`: lines taken, lines
skipped, the queue's length after the call, and the frame `+N` counts from.
The queue is kept sorted, so a vblank's cost doesn't grow with it.

### Input state

`query-xemu-input` returns `{frame, ports, keys, queued}`, reading the pipe
first and changing nothing. `ports` gives each port's source and what its
pad reports from the next vblank: `buttons` (`a`, `dpad-up`, `white`, ...),
`lx`/`ly`/`rx`/`ry` and `lt`/`rt`, raw (a pipe value v reads as round(v ×
32767); the guest reads triggers shifted right by 7). For the keyboard pad
that is the pipe's state after the lines due at that vblank, plus the host
keyboard where it counts; for a gamepad, its last reading. `keys` lists the
scancodes the pipe holds then, `queued` the waiting lines, each with the
`frame` of the vblank that applies it (`frame + 1` is the next).

### Input recording

`XEMU_INPUT_RECORD=<file>` writes, every vblank, the changes in what one pad
(`XEMU_INPUT_RECORD_PORT=1-4`, default the first bound port) gave the guest,
as pipe lines tagged `+N` from the last load, with a `# load` section per
load. Buttons become `down`/`up` of their keyboard-map scancodes, sticks and
triggers values that give back the same 16-bit reading (a stick's -32768 is
`-1.000030518`), so gamepad sessions replay through the keyboard pad. While
recording, pads change only at vblanks, and stderr warns once per port when
another port's pad gets input. To replay: keyboard pad on that port with the
same keyboard map, load the state paused, write the section, advance. Icount
recordings replay bit-identically; normal-mode ones replay identically from
replay to replay and close to, not exactly like, the original.

### Pads across loads, and rebinding them

A pad reads its port's host source at every guest poll, so the bound source
keeps driving it across `loadvm`, checkpoint restores and `system_reset`.
The state must have a pad of the same type (Duke or S) on that port (port 2
for states saved with the keyboard pad there), or see [Loads that plug
missing pads](#loads-that-plug-missing-pads).

`xemu-input-bind {"port": 2, "source": "gamepad", "guid": "<32 hex
digits>"}` changes a port's source without unplugging (`"keyboard"` or
`"pipe"`: the keyboard pad; `"none"`: neutral). A source bound elsewhere
moves (that port keeps its pad); a port with no pad gets one (not under
`XEMU_ICOUNT_LIVE_PADS=1`). The port is then pinned: its pad stays through
gamepad disconnects (neutral meanwhile), the gamepad drives it again when it
reconnects, auto-binding leaves it alone, and choosing a device for it in
settings ends the pin. Nothing is saved to the
config. Under icount and while recording it applies at the next vblank
(`"pending": true` until then); an unopened GUID is waited for.
`query-xemu-input-bindings` lists the ports (source, pad, type, pinned,
pending) and the open gamepads with their GUIDs; gamepads open only in
normal mode and under `XEMU_ICOUNT_LIVE_PADS=1`.

## Frames and deterministic mode

### Frames and frame advance

Frames count vblanks while the VM runs. `query-xemu-frame` returns
`{frame, running, vblank-ns}`. `xemu-frame-advance {"frames": N}` (default
1; `0` just pauses) runs N frames, pauses and returns. It also resumes a
gdbstub-stopped VM from its stop (lift a breakpoint under the PC first, or
use `Qxemu.stepover:1`). It ends early as below; after a stop it didn't
make, the VM stays as that stop left it:

| Reply | Cause |
|---|---|
| `debug-stop: true`, `stopped: debug` | A gdbstub breakpoint or step |
| `stopped: paused` | A gdbstub client attaches, sends ^C or any byte while the VM runs |
| `stopped: shutdown` | SIGTERM |
| `stopped: restore-vm` | A save or load failed and left the VM stopped (a failed HMP `loadvm` or `snapshot-load`) until `cont` or a load that works |
| `timed-out: true` | `"timeout-ms": T` (host ms) ran out; the VM pauses wherever it is, not at a deterministic point |

Saves and loads that work (HMP `savevm`, `loadvm`, the Snapshots menu) don't
end an advance. `timeout-ms` also works on `xemu-run-until` and `xemu-run`
(`reason: timeout`); a host deadlock holding the BQL still hangs. Real-time
replays land within a frame or two; deterministic mode makes them exact.

`"trace": [{"address": A, "size": S}, ...]` adds `trace` to the reply: a
sample every `"trace-every"` vblanks (default 1, the last always included)
with each region's bytes in hex, read before the guest sees that vblank.
`"virtual": true` reads through CPU 0's page tables; a region RAM doesn't
fully back reads `""`. Up to 256 regions of 1-4096 bytes ending at or below
4 GiB; exact under icount.

### Deterministic mode

`-icount shift=2,align=off,sleep=off` (with `XEMU_HEADLESS=null`) makes
guest time follow executed instructions, as fast as the host allows. The
same savestate, frame-tagged input and QMP calls then give bit-identical
guest RAM after N frames, across runs, processes and pause points. Launch
with `-S`, load, write input tagged `+N` while paused (lines queued before a
load are dropped), advance. Keep the network off and bind no XMU. Under
icount:

- vblank, the APU (8 frames per 5.33 ms of guest time), PFIFO, PGRAPH, IDE
  completions and the RTC follow guest time; the RTC starts at 2001-01-01
  unless `-rtc` says otherwise. `xemu-frame-advance` stops on the vblank
  itself.
- Pads change only at vblanks and only from the pipe (host input is ignored
  unless `XEMU_ICOUNT_LIVE_PADS=1`). Untagged lines apply at the next
  vblank; an `@N` line read after frame N is warned about. Frame numbers
  count this process's frames, so use `+N` after a load.
- Host audio is off, windowed too. OpenGL is not deterministic.
- An icount save carries the host-side state the run depends on (resampler,
  filter and HRTF history, APU schedule, AC'97, RTC sub-second phase, field
  pin, GPU vertex array and lighting state, pad reports, held pipe keys and
  axes), so a run continued past `savevm` or `xemu-checkpoint-save` and
  every load of it go on identically. A normal-mode save starts that state
  as a fresh process does, with neutral pads.
- Savestates cross modes: normal-mode states replay as above; icount states
  load in normal mode with guest time and the RTC continuing from the save
  (v0.14 and earlier read their RTC about 56 years ahead: load and re-save
  in normal mode first). Save with `sleep=off`; a state saved with sleep on
  or `shift=auto` loads anywhere but drops its warp deadlines.
- `shift` is 2^N ns of guest time per instruction: 1 is about a 733 MHz
  Pentium III; 2 runs ~1.5x faster and suits games that aren't CPU-bound.

**Offscreen Vulkan** (`XEMU_HEADLESS=1`) is deterministic too, paused
screendumps included, at about half null's speed; matches hold only on the
same Mac, macOS and MoltenVK (offscreen always uses it). GPU reports reach
RAM lazily (first guest or device touch, a hook, trace or value-stop read, a
pause, save or load). Hash its RAM with `xemu-dump-ram` `"gpu-sync": false`:
`gpu-sync`, `pmemsave` and `xemu-checkpoint-save` write GPU surfaces back
and so change later RAM. For a reproducible picture of a null run's frame F:
`savevm` there, load it in a short offscreen icount run, advance a few
frames, `screendump`.

### Live pads under icount

`XEMU_ICOUNT_LIVE_PADS=1` with `-icount shift=2,align=on,sleep=on` lets host
gamepads, and the host keyboard on the keyboard pad, drive their ports while
the run stays deterministic.

- They are read once per vblank, so `XEMU_INPUT_RECORD`
  (`XEMU_INPUT_RECORD_PORT` set to the pad's port) holds exactly what the
  guest got: its last section, written after a load of the same state in
  another process (`sleep=off` for speed), ends on the session's RAM. The
  session's `savevm`s are icount (`sleep=off`) saves.
- Keep every live source on the recorded port (the keyboard pad too, when
  the window has focus); stderr warns once per port otherwise. Windowed
  sessions replay bit-exactly on macOS only; elsewhere record headless.
- `align=on` keeps the guest within 3 ms ahead and 0.1 s behind real time; a
  scene the host can't keep up with plays slower, and stderr says `xemu:
  icount live pads: the guest ran at N% of real time over the last 10 s`.
  Idle time warps as with `sleep=off`, but only once real time gets there.
- A connecting gamepad drives, from the next vblank, the port waiting for
  it, else its configured or a free port, if that port has a pad; a
  disconnecting one leaves its pad neutral and its port waiting for it.
  Nothing is plugged or unplugged (except that a pad with a memory unit on
  its hub unplugs at a vblank, as in normal mode), so `xemu-input-bind`
  refuses a port with no pad. Rebind with `xemu-input-bind`, not the
  settings menu.

## Checkpoints

### Checkpoints in host memory

`xemu-checkpoint-save {"name": "c"}` keeps the whole machine state (what
`savevm` writes) in host memory: the device state (about 6 MB) plus each
nonzero RAM page; the qcow2 is not written.
`xemu-checkpoint-restore {"name": "c"}` loads it as `loadvm` does (reset,
load, guest disk writes since put back, runs on only if it was running), any
number of times, with the same determinism. `query-xemu-checkpoints` lists
them; `xemu-checkpoint-delete {"name": "c"}` frees one. At 128 MiB a save
takes about 10 ms, a restore about 8 ms.

### Branching from checkpoints

- `"base": "c0"` on a save stores only the pages that differ from `c0` and
  shares the rest (they survive deleting or replacing `c0`): after 16 frames
  about 1,600 pages plus device state, in about 4 ms.
- `"incremental": true` on a restore writes back only the pages written
  since the last checkpoint save or restore (QEMU's migration dirty log,
  which this and `base` start) plus those the two checkpoints hold
  differently, ending on the same machine as a full restore in 2-5 ms. The
  first one, and the first after a `loadvm`, incoming migration or failed
  restore, compares every page.
- Replies carry `ram-pages` (pages it holds itself) and `base`; restores add
  `pages-checked` and `pages-copied`. `"verify": true` on either also
  compares every page the log didn't name: `pages-missed` must be 0 (another
  process writing through `XEMU_RAM_SHM` breaks it). The log stops when the
  last checkpoint is deleted.

### Park and resume a machine

Without touching the HDD's snapshots: `stop`,
`migrate {"uri": "file:/path"}`, poll `query-migrate` for `completed`.
Resume in a new xemu started with `-incoming defer` on an unchanged copy of
the same HDD: wait for `xemu: ready`,
`migrate-incoming {"uri": "file:/path"}`, wait for `xemu: state loaded`,
`cont`.

## Hooks and run-until

An **address expression** `E` is a register or constant plus offsets with up
to 8 `[ ]` pointer reads through CPU 0's page tables: `"esp+0x14"`,
`"[[ecx+0x20]]+0x15c"`. A **condition** is
`{"reg": "ecx", "op": "eq", "value": V}` or `{"addr": E, "size": 1|2|4|8,
"type": "u"|"s"|"f", "op": "eq"|"ne"|"lt"|"gt"|"le"|"ge", "value": V}`;
unreadable memory is false.

### Run until a guest PC

`xemu-run-until {"until": [{"pc": P, "cond": [...]}, ...], "hits": N,
"frames": F}` runs a paused or gdbstub-stopped VM until it reaches one of
the PCs with all its conditions true for the Nth time (default 1), or for F
vblanks, and pauses it before that instruction, outside the gdbstub (an
attaching debugger finds it halted, with no stop to report).

- `reason` is `hit` (with `pc`, the entry's `index`, `hits`, `frame`,
  `regs`: eax..edi, eip, eflags), `budget`, `debug` (a gdbstub breakpoint
  first), `stopped` (another stop, as for frame advance), `timeout`,
  `mismatch` or `empty` (hooks, below).
- The start instruction doesn't count, so repeated calls walk hit to hit.
  `trace` and `trace-every` work as for `xemu-frame-advance`. Under icount
  it is exact, and a run cut into hits ends on the same RAM as one straight
  advance.
- PCs are compiled into the translated code; gdbstub breakpoints work beside
  them. The first call at a PC retranslates its page once, as does the first
  run through each later mapping of it; later calls cost about 1-2 ms.

### Collect at guest PCs

`xemu-hooks-set {"hooks": [{"pcs": [P, ...], "cond": [...], "key": {"addr":
E, "size": N}, "collect": [{"addr": E, "size": N}, ...], "regs": true,
"expect": [...]}]}` (VM paused; at most 64 hooks) copies guest memory at
each hit, before the instruction, and leaves the VM running. It returns each
hook's `id`; `xemu-hooks-clear {"ids": [...]}` (default all) removes them,
and `query-xemu-hooks` lists them with their `hits`.

- A hit is an arrival at one of `pcs` (1-16, numbered together, e.g. a
  function's RETs) with `cond` true and, given a `key` (1-64 bytes, e.g. an
  event clock at `"ecx+0x18"`), the key changed since the hook's last hit.
- `collect`: up to 64 regions of 1-4096 bytes, 64 KiB a hit.
- The next `xemu-frame-advance` or `xemu-run-until` reply carries `hooks`:
  `records` in execution order (`hook`, `pc`, `frame`, `hit` from 0, `data`
  with a hex string per region, `""` if unreadable, `regs` if asked) and
  `dropped` (hits past 262,144 waiting records or 16 MiB). `"frames": 0`
  just fetches them, also from a gdbstub-stopped VM. `xemu-hooks-drain`
  returns `{records, dropped}` without pausing: a running VM's hooks keep
  recording from where the call cut, so polling it keeps every hit exactly
  once. Sent during an advance, it waits for the advance's reply, which
  takes them.
- `expect`: one hex string per hit that the concatenated collected bytes
  must start with (`??` any byte, `""` nothing). A mismatch marks the record
  and stops the advance before the instruction (`hooks` gets `stop`;
  `xemu-run-until` returns `mismatch` with `pc` and `regs`); `"stop": false`
  only marks it. Resuming repeats and misses no hit.
- Under icount records are exact, and collect hooks never change RAM.
  gdbstub breakpoints and run-until at the same PC coexist; the hook still
  sees each arrival once. Hooks, hit numbers, keys and queues survive loads:
  clear and set them again to start over.

### Write at guest PCs

`"write": [{"addr": E, "size": N}, ...]` with `"set": [hex, ...]` writes the
queue's next entry over the concatenated regions at each hit, before the
instruction (`??` keeps a byte, a shorter string writes less, `""` nothing;
with a `key`, a repeat in one tick takes no entry).

- Only guest RAM is written, as a gdbstub write does (translated code there
  is dropped; page permissions don't apply); regions not in RAM are skipped
  (`"unwritten": true`). Records add `set` (entry index) or `"empty": true`.
- `"stop-empty": true` stops before the first hit with no entry left
  (`stop`; `xemu-run-until` reason `empty`).
  `xemu-hooks-push {"id": N, "set": [...]}` appends entries while the VM is
  stopped (up to 1,048,576 or 64 MiB waiting); a hit stopped on an empty
  queue takes the first pushed entry on resume and is recorded again with
  it.
- A mismatch stop comes before the hook's write, which lands on resume, so a
  cut run writes what an uncut one does.

The Azurik harness replays an input string with one `xemu-frame-advance` and
three hooks (JSON numbers are decimal): at the player update's entry
(`ENTRY` = 0x8C230) `{"pcs": [ENTRY], "cond": [{"reg": "ecx", "op": "eq",
"value": PLAYER}], "key": {"addr": "ecx+0x18", "size": 8}, "write":
[{"addr": PAD, "size": 72}], "set": [...], "stop-empty": true}` writes the
pad's 18 floats (`PAD`: the active ControllerState, resolved once); at the
camera read (`CAMREAD` = 0x85276) `{"pcs": [CAMREAD], "key": ..., "write":
[{"addr": "esp+0x14", "size": 8}], "set": [...]}`; and a collect hook at the
update's RETs observes. Under icount this equals writing the same bytes over
the gdbstub at the same instructions.

### One-call candidate runs

`xemu-run {"checkpoint": "c", "input": ["+0 axis ly 1.0", "+12 down 81",
...], "frames": F, "trace": [...], "trace-every": K, "stop-when": [...],
"hits": N}` on a stopped VM (paused, prelaunch or debugger-held) restores
the checkpoint (optional; incremental unless `"incremental": false`), queues
`input` as pipe lines arriving right after the restore (pipe syntax; `+N`
counts vblanks from the restore; blank and `#` lines are skipped; a
malformed line refuses the call), then runs as `xemu-run-until` for F
vblanks or until a `stop-when` entry fires, and pauses. Nothing runs in
between, so no pipe write races it.

- `stop-when` entries: `{"pc": P, "cond": [...]}` (`hits` counts them,
  default 1);
  `{"addr": E, "size": 1|2|4|8, "type": "u"|"s"|"f", "lo": A, "hi": B}`
  (either bound optional), firing at the first vblank the value is outside
  [A, B] (NaN or unreadable counts as outside);
  `{"addr": E, ..., "change": true}`, firing when its bytes differ from the
  start. Values are read at each vblank, where trace samples are, so a
  value stop ends the run on that vblank, which the trace includes.
- The reply is `xemu-run-until`'s (`reason` may also be `value`; `index` is
  the `stop-when` entry) plus `frames` run and, at a value stop, `data` and
  `previous` (hex, `""` if unreadable).
- Under icount the same checkpoint, input and stops give the same RAM, reply
  and trace (except `frame`, which never rewinds) while no `xemu-hooks-set`
  hook is installed (hooks keep their state across the restore). A run
  stopped and continued ends on the same RAM as an uncut one.

## RAM digests, dumps and watch logs

### RAM digests

`xemu-ram-digest` returns `sha256` of all guest RAM (as `xemu-dump-ram`'s
file hashes) and `ram-size`, with no file and no GPU write-back, pausing a
running VM (~55 ms at 128 MiB). `"pages": true` adds `page-hashes`, 16 hex
digits per 4 KiB page in address order (the first 8 bytes of its sha256).
`"algorithm": "xxh3-128"` returns `xxh3-128` instead (32 hex digits, as
`xxh128sum` prints; ~4 ms) and per-page XXH3-64: not cryptographic, but
equal across identical runs. `"against": "<checkpoint>"` returns `differ`,
the page numbers (address / 4096) that differ from that checkpoint (`sha256`
only with `"sha256": true`).

### Bulk RAM dumps

`xemu-dump-ram {"filename": "/tmp/ram.bin"}` writes all of guest RAM in one
write, pausing the VM unless `"pause": false`. `"address"`/`"size"` select a
physical range, or with `"virtual": true` CPU 0 virtual addresses (unbacked
pages read as zeros, counted in `unmapped`). `"gpu-sync"` (default on) first
writes GPU surfaces back, as `savevm` does. `pmemsave` and `memsave` work
too, 1 KiB at a time.

### Zero-copy RAM

`XEMU_RAM_SHM=<name>` (or `[automation] ram_shm`) puts guest RAM in named
shared memory: POSIX shm `/<name>` on macOS and Linux, a named file mapping
on Windows (alive while any process has it open, readers included: close
them before relaunching, or the new xemu keeps private RAM). In Python:
`multiprocessing.shared_memory.SharedMemory("<name>")`, then
`resource_tracker.unregister` (or `track=False` from 3.13) so Python doesn't
unlink it. stderr: `xemu: guest RAM in shared memory /<name>`. Savestates
are unaffected.

- The holder keeps an flock on `/tmp/xemu-shm-<uid>/<name>.lock`. A name a
  live xemu holds is never taken: the newcomer keeps private RAM and says
  `xemu: XEMU_RAM_SHM: /<name> is held by a live xemu (pid N); guest RAM
  stays private`, plus an in-app notification.
- A name nobody holds (a crashed run) is replaced
  (`replaced a stale object`) unless its bytes still change; that is all
  that protects builds from before the lock. While such a build is paused
  or stopped (`-S`, a gdbstub stop) its name is replaced, and its exit
  unlinks the name whoever holds it by then.
- At exit the name is unlinked only if it is still that xemu's object. Other
  failures to create it end the launch.

### Memory access log

`xemu-watch-start {"ranges": [{"address": A, "size": S, "access": "w"}],
"max": M}` logs, without stopping, every CPU load or store overlapping a
range while the VM runs. `access` is `r`, `w` or `rw`; up to 64 ranges of 1
byte to 16 MiB below 4 GiB, physical by default (matching every mapping of
that RAM, or MMIO outside RAM) or `"virtual": true`. `xemu-watch-drain`
returns the hits so far and keeps logging; `xemu-watch-stop` returns them
and stops. Hits past `max` (default 100000, at most 1000000) only count, in
`dropped`.

- A hit is `{frame, pc, address, physical, size, access, value}`: `value` is
  the bytes in hex (at most 64) a read read or a write left; MMIO hits have
  none. A hit through NV2A BAR1's window at `0xF0000000` matches at its RAM
  address and adds `ram-address` (its offset in `XEMU_RAM_SHM` and dumps);
  `physical` stays the bus address. Hits at RAM's own addresses have no
  `ram-address`. On 128 MiB the window's upper half needs the bridge
  widened (next section).
- Under icount the log is the same every run and RAM is as without it; start
  it paused for an exact first instruction. Not logged: device DMA,
  page-table walks. A page-crossing access gives one hit per page. Watched
  pages take the slow path. gdbstub watchpoints coexist.

### NV2A BAR1's window onto RAM

The kernel maps VA `0xF0000000`-`0xF7FFFFFF` onto NV2A BAR1, an alias of all
of RAM, but the AGP bridge (`00:1e.0`) forwards only its windows, and the
kernel cuts the prefetchable one to `0xF0000000`-`0xF3FFFFFF`, so on 128 MiB
stores to the upper half (RAM `0x04000000`-`0x07FFFFFF`) are dropped and
gdbstub reads there fail. The guest can widen it (PCI config `0x8000F024` =
`0xF7F0F000`, kept in savestates), or `XEMU_AGP_FULL_DECODE=1` (stderr
`xemu: XEMU_AGP_FULL_DECODE: ...`; off by default, since real hardware
doesn't) forwards what the NV2A decodes outside the windows, changing no
registers, savestates or replays. Through the window a store counts as a
RAM store everywhere (dirty log, digests, dumps, `XEMU_RAM_SHM`, translated
code, physical watch ranges). Readers that follow page tables (hook reads,
`xemu-run` value stops, virtual trace regions and dumps) see the whole
window unmapped; with `XEMU_AGP_FULL_DECODE=1` they read the RAM it
reaches, in the lower half `0xF0000000`-`0xF3FFFFFF` too and at 64 MiB as
well. Without the opt-in, read such data at its RAM address.

## Block, voice and audio logs

| Variable (from launch) | QMP | Writes |
|---|---|---|
| `XEMU_BLOCK_LOG=<file>` | `xemu-block-log-start`, `-stop` | One row per IDE or ATAPI read, write, flush or discard |
| `XEMU_VOICE_LOG=<file>` | `xemu-voice-log-start`, `-stop` | One row per APU voice event |
| `XEMU_AUDIO_WAV=<file>` | `xemu-audio-wav-start`, `-stop` | What xemu would play |

`-start` takes `{"filename": F}` (logs also `"append": true`); `-stop`
returns `{filename, rows}` (WAV: `samples`). One of each at a time. The
files are deterministic under icount; the voice log and WAV change nothing
the guest sees, and stopped and restarted at a frame they split the full
run's file there. The logs are tab-separated under a header line, flushed
when the VM pauses, at stop, at exit and at a row 250 ms or more after the
last flush. `frame` is `query-xemu-frame`'s, `guest_ns` the virtual clock.

### Per-drive I/O log

```
frame drive op lba sectors bytes status cmd guest_ns
```

`frame` and `guest_ns` are taken when the command is issued. `drive` is the
`query-blockstats` name (`ide0-hd0`, `ide0-cd1`). `op` is `r`, `w`, `f`
(flush) or `d` (discard). `lba` and `sectors` are what was asked, in
512-byte HDD or 2048-byte DVD sectors. `bytes` is what blockstats counted,
so rows add up to blockstats deltas over any window. `status` is `ok`, `err`
or `open` (still running at close). `cmd` is the ATA or ATAPI packet opcode
in hex. No data is logged. ATAPI DMA replies (REQUEST SENSE, READ CAPACITY)
are `r` rows with `lba` -1. Commands already running at start are not
logged. A cold boot writes ~70,000 rows, at 1-2% more host time.

### Voice log

```
frame guest_ns voice event why phys bytes fmt ch flags ba cbo lbo ebo pitch hz vol bins wav_pos runs
```

| Column | Meaning |
|---|---|
| `event` | `start` (VOICE_ON), `release`, `pause`, `resume`, `stop`. Only `start` rows are written for a voice that isn't playing; a voice already playing when the log opens has no `start` |
| `why` | `guest` (VOICE_OFF), `end` (buffer without loop, or stream, ran out), `env` (envelope release done), `stream` (stream without PERSIST left before release) |
| `phys`, `bytes` | First sample byte's physical address; data length (buffer: BA through sample EBO; stream: its current SSL segment) |
| `runs` | Physical extents `0xADDR+LEN` in order, contiguous pages merged (at most 256, then `...`): `xemu-dump-ram` of each gives the exact bytes played |
| `fmt`, `ch`, `flags` | `u8`, `s16`, `s24`, `s32`, `adpcm`; 1 or 2; `l`(oop) `s`(tream) `p`(ersist) `m`(ultipass) or `-` |
| `ba`, `cbo`, `lbo`, `ebo` | Buffer scatter-gather offset; current, loop and end sample offsets |
| `pitch`, `hz` | Raw signed pitch (1/4096 octave from 48 kHz); the sample rate it gives |
| `vol`, `bins` | 8 volume slots as hex attenuation in 1/64 dB (`fff` silent); their mixbins, 0-63 (3D voices send slots 0-3 to the HRTF submixes) |
| `wav_pos` | Sample of the open WAV the next APU frame lands on, else -1; with `audio.use_dsp` a voice is heard from about `wav_pos + 259` |

Pitch and volume are the values at the event.

### Audio to a WAV

48 kHz stereo 16-bit PCM of what xemu would play, before `volume_limit`, one
EP period (256 samples) at a time; nothing plays aloud. With
`XEMU_AUDIO_WAV` xemu opens no host audio device, windowed too (stderr
`xemu: audio WAV: <file> (no host audio device)`); a QMP-started WAV leaves
the host device as it was. Periods the APU doesn't run leave no gap, so WAV
time is APU time. Header sizes are updated every second of audio, at pause,
at stop and at exit.

## Trace recording

`XEMU_TRACE_RECORD=<file>` writes a line per vblank while the VM runs, in
any mode:

```
# xemu trace recording: <n> <frame> <hex per region>; ...
# regions 0x1000:16 v0x80012340:64 v[[0x83a6e2c0+0x20]+0x154]:4
# load frame=1234
1 1235 00112233... 8899aabb... 01000000
```

- `n` counts vblanks from the last load (the input recording's `+N`);
  `frame` is the process's counter. Bytes are lowercase hex, `-` for a
  region RAM doesn't fully back. Each load writes a `# load` line, each
  region change a `# regions` line.
- `XEMU_TRACE_REGIONS=0x157b000:256,v0x10000:64` sets the regions (`v`:
  through CPU 0's page tables; 1-4096 bytes ending at or below 4 GiB, at
  most 256). `xemu-trace-regions {"regions": [{"address": A, "size": S,
  "virtual": true}]}` replaces them from the next vblank (an empty list
  leaves only the counts).
- An address may be an address expression
  (`'[[[[0x1BF63C]+0x2C]+0xC0]+0x180]+0x38:12'`, or `{"addr": E, "size": N}`
  over QMP): virtual, re-evaluated every line, so it follows a re-allocated
  pointer from the next line. Its field is `?` when a dereference reads
  non-RAM (a null pointer) and `-` when the target bytes aren't all RAM.
  `# regions` writes it as `v<expression>:<size>` without blanks, which
  parses back as `XEMU_TRACE_REGIONS`.
- Under icount the line is taken at the vblank, before the guest sees it,
  where `xemu-frame-advance` samples, so recording never changes the run.
  With a GPU renderer an unread report slot may hold old or new bytes
  (reports are lazy), while a traced frame advance delivers pending reports
  first: compare report slots recording to recording. In normal mode the
  sample is taken between guest code blocks around the vblank interrupt,
  never mid-instruction; where the guest is then depends on host timing.

## Savestate loading

### Loads that plug missing pads

With `XEMU_LOADVM_ADD_PADS=1`, a load (`loadvm`, `snapshot-load`, the
Snapshots menu, CLI `-loadvm`, `xemu-checkpoint-restore`) that stops at a
pad hub or pad the machine lacks
(`Unknown section or instance 'pci.0:02.0/1.3/usb-hub'`) plugs it with no
host source and loads again; a pad of the other type (Duke or S) is swapped
for the state's. stderr: `xemu: loadvm: plugged ...`. Each attempt resets
and loads the whole state, so the result equals a load with the pad present.
A source bound to that port later drives it without a replug. Memory units
are never plugged. CLI `-loadvm` runs once the config's pads are bound, just
before `xemu: ready` (and before the VM starts), so it equals a `loadvm`
sent after `xemu: ready`.

### Snapshots and the disc

A `savevm` records the tray's disc path and a fingerprint (size and XXH3-128
of every byte; older builds ignore it). Each image is hashed once per
process (cached by path, size, mtime, ctime and inode), on up to 8 threads;
the first `savevm` with a disc prints
`xemu: disc fingerprint: <path>: <n> bytes, xxh3-128 <hex> (<t> s)`. A read
projected past 10 s is given up, leaving no fingerprint.

`loadvm`, `snapshot-load` and CLI `-loadvm` compare no discs and load with
whatever is in the tray. Only the Snapshots menu (and F5-F8) checks: the
same image under another path loads without a dialog and says so
(`xemu: snapshot '<tag>': loading with the tray's disc <tray>, the same as
its disc <saved> (<how>)`). A snapshot saved before fingerprints is compared
with its saved path's file while that file is unchanged since the save
(mtime and ctime at or before the snapshot's date); on Windows such
snapshots always ask. Anything else asks
(`... the tray holds <tray> (<reason>); asking`).

In the dialog, Yes opens the saved image before the tray's disc comes out,
so a failed Yes leaves the tray as it was (`...; the tray keeps <tray>; not
loaded`). A saved image that no longer exists is marked missing and gets no
Yes (Load Anyway or Cancel, Cancel focused). With a disc in the tray the
dialog also offers Load With Tray Disc, which loads as No does and logs
`xemu: snapshot '<tag>': loading with the tray's disc <tray> instead of its
disc <saved>; RAM keeps that disc's code until a reset`.

### Savestates and RAM size

A savestate loads only into the RAM size it was saved with. Otherwise
`loadvm` fails (`Size mismatch: pc.ram: 0x8000000 != 0x4000000 ...`) with a
last line naming both sizes and the fix: `xemu: this snapshot was saved with
128 MiB of guest RAM; this machine has 64 MiB (mem_limit in xemu.toml, or -m
on the command line). To load it, set mem_limit '128' in xemu.toml or pass
-m 128, ...`. The line reaches the monitor reply, stderr for `-loadvm` and
the menu's error popup.

`XEMU_LOADVM_SHRINK_RAM=1` loads a snapshot into a smaller machine if every
page above its size is zero, except the 16 bytes the BIOS memory test leaves
in each of `0x04555548-0x0455557B` and `0x06555548-0x0655557B`: stderr
`xemu: XEMU_LOADVM_SHRINK_RAM: loaded a 128 MiB snapshot into 64 MiB of
guest RAM (above it: N zero pages, M with the BIOS memory test's
footprints)`. Other data refuses the load, naming the first page that holds
it. Azurik's 128 MiB states load this way and replay deterministically to
the low 64 MiB of the 128 MiB run.

### Load check

`XEMU_LOAD_VERIFY=1` copies guest RAM at every load (`loadvm`,
`xemu-checkpoint-restore`, incoming migration), and when the VM next starts
(or the next load begins) reports pages changed meanwhile: `xemu: load
verify: RAM changed between the load and the VM start (frame F): N pages
0x... 0x...` (up to 8 listed). Your own writes in that window (gdbstub `M`,
a `gpu-sync` dump under Vulkan) count; what the start itself does on the
vCPU does not. It costs a RAM-sized copy and a few ms per load and start.

## gdbstub

- **RAM and ROM only.** `m`, `M`, `x` and `qxemu.read`, virtual or in
  `Qqemu.PhyMemMode:1`, return `E14` (or stop at the readable prefix) on
  device MMIO (NV2A
  `0xFD000000`-`0xFDFFFFFF`, APU, USB, NIC) without touching the device, and
  on unbacked physical addresses. HMP `x`/`xp` and QMP `pmemsave`/`memsave`
  still reach devices.
- **Reads.** An `m` read that runs into an unmapped page (heap guard pages)
  returns the readable prefix. `m` may ask for up to 4 MiB; the binary
  `x addr,len` (GDB 16, `binary-upload+`) halves the bytes on the wire.
  `qxemu.read:addr,len;addr,len;...` (`qxemu.read+`; hex, up to 4 MiB in
  all) answers many `m` reads in one round trip, one `;`-separated field
  each (hex bytes, the readable prefix, or `E14`); malformed requests get
  `E22`.
- **Step-over.** `Qxemu.stepover:1` (or `xemu.stepover+` in the client's
  `qSupported`; the stub advertises `Qxemu.stepover+`) makes a resume from a
  halt on an armed `Z0` (`c`, `vCont;c`, QMP `cont` or `xemu-frame-advance`)
  run the instruction under the PC first, single-stepped with the `s` flags
  (no IRQs, no timers), so the guest runs exactly as under
  `z0`/`s`/`Z0`/`c`. Off by default and per connection; `Qxemu.stepover:0`
  turns it off, `qxemu.stepover` reads it. `s` steps over a breakpoint under
  the PC in every build.
- **Breakpoints** (`Z0`/`Z1`) are compiled into the translated code, so the
  rest of the page runs at full speed. Only the first `Z0` at a PC
  retranslates its page; `z0` and a later `Z0` there invalidate nothing.
  DR7 breakpoints the guest sets itself still single-step their page.
- **Watchpoints** `Z2` (write), `Z3` (read), `Z4` (access) stop the VM after
  the access, in normal and deterministic mode, with QMP answering
  throughout; under icount the RAM is the same as without them. Each stop
  retranslates the block that hit. A hit on an
  interrupt or exception frame push stops at the handler's first
  instruction. QMP `cont` or `xemu-frame-advance` (`debug-stop: true` at
  each hit) runs to the next hit as `c` does.

## Guest checks and per-run settings

### Console language

`XEMU_LANGUAGE=<lang>` (from launch; set it per run) or
`xemu-set-language {"language": "<lang>"}` (VM stopped; `"off"` restores the
EEPROM's own; no argument only reports) sets the language the guest reads:
`en ja de fr es it ko zh pt`, a name (`french`) or an XC_LANGUAGE number.

- EEPROM reads return it with a matching checksum, so a boot or reset reads
  it. The call and every later load also set it in the kernel's copy of the
  EEPROM in guest RAM (8 bytes on one page; a state already in that language
  loads unchanged, replays stay exact).
- The device, the EEPROM file and savestates keep the EEPROM's own language:
  guest EEPROM writes land with it, and the file is never written while the
  language is set (stderr says so once). stderr names the language at launch
  and the copy each load set.
- A title that asks once per boot keeps what it got: Azurik asks in `main()`
  (USA `0x58EA8`, Europe `0x58EC8`), so a loaded state's speech keeps its
  session's language until the next boot. Europe also asks at each Bink
  movie (`0xD03BC`, to pick its audio track), so its movies follow the
  setting.

### Fixed clock

`-rtc base=2001-01-01T00:00:00` pins the RTC on a fresh boot. After `loadvm`
the RTC is the snapshot's plus the wall time since it was saved; with
`-rtc clock=vm`, no wall time is added.

### Strict CPUID

On the Xbox's `pentium3` (MMX and SSE, no SSE2) QEMU runs every SSE2
instruction except MFENCE and MOVNTI, and also SSE3's FISTTP and LFENCE;
all of these fault on a real Xbox. `XEMU_STRICT_CPUID=1` makes them
raise #UD (the legacy SSE/SSE2/SSE3/MMX opcodes by prefix, FISTTP,
LFENCE). Each fault prints
`xemu: strict CPUID: #UD at <linear pc>: F2 0F 58 C1 needs SSE2` and sends
`XEMU_CPUID_FAULT {pc, feature, opcode, count}` before the guest's #UD
handler runs (the first 32 per launch; the kernel usually bugchecks next).
Off, translation is unchanged. Retail Azurik never trips it; it is
deterministic under icount.

### Guest bugchecks

`XEMU_GUEST_BUGCHECK=1` names a boot that stops in the kernel's halt (the
4627 kernel's `cli; hlt; jmp` at 0x800151ED; EIP reads 0x800151EF with every
I/O counter frozen).

- At each load, and at a boot's second vblank, xemu finds KeBugCheckEx
  (export 96; KeBugCheck, 95, calls it) in the image at 0x80010000 and hooks
  it (`xemu: guest bugcheck: KeBugCheckEx at 0x80019442 hooked` on 4627).
- Each call prints `xemu: guest bugcheck 0x<code> (<p1>, <p2>, <p3>, <p4>)
  from <return address>` plus a register line, and sends
  `XEMU_GUEST_BUGCHECK {code, params, caller, count, regs, stack}` (`stack`:
  4 KiB from ESP in hex; for a 0x1E it holds the trap frame and context
  record, the faulting EIP (p2) followed by CS 0x0008). A HLT with
  interrupts disabled prints
  `xemu: guest halted with interrupts disabled at <eip> (esp <esp>)` and
  sends `XEMU_GUEST_HALTED {eip, esp, count}`. The first 16 of each per
  launch are reported. Both only read guest state.
- Without it, a halted 4627 guest still holds the code and parameters at
  0x8003AB20-0x8003AB33 (five dwords, zero until a bugcheck), with EAX =
  code and ECX/EDX/ESI/EDI = p1-p4 at the halt (read them with `qxemu.read`
  or `g`).

## Profiling counters

### Host-time profile

`query-xemu-host-profile` returns counters cumulative since launch:
`translate-ns`/`translations` (translating guest code), `apu-ns`/`apu-ticks`
(APU ticks under icount), `idle-ns`/`idle-warps` (guest time a halted vCPU
skipped under `sleep=off`). Diff two queries for an interval. Disk latency
is in `query-blockstats` (`rd/wr/flush_total_time_ns`).

`scripts/bench-det.py --timeline APP` times a cold boot under icount phase
by phase (kernel to steady play), splitting host time into vCPU,
translation, APU, disk, qcow2 growth and other threads; `--help` lists its
options. `--hdd-io` runs a qemu-io command on the scratch HDD before
power-on: `"write -z 0x800 0x200"` empties the cache-partition table, and
`"reopen -o lazy-refcounts=on"` drops the qcow2's ordering flushes while it
allocates (a fifth of that boot's host time). For any scratch image, QMP
`human-monitor-command` `qemu-io ide0-hd0 "reopen -o lazy-refcounts=on"`
before power-on does the same; a killed xemu leaves the image marked dirty
and the next open repairs it.

### NV2A workload counters

`xemu-frame-advance {"frames": N, "gpu-counters": true}` adds
`gpu-counters`, the NV2A work of the advance
(`"gpu-counters-per-flip": true` adds a set per flip).
`xemu-gpu-counters-start` (`{"per-flip": true}` optional),
`query-xemu-gpu-counters` and `xemu-gpu-counters-stop` count across any run;
one counting run at a time. The reply is
`{flips, total, max, tail, per-flip, per-flip-dropped}`: a flip is a guest
`FLIP_STALL` (the first holds only the work since counting began), `total`
sums everything counted, `max` is each counter's largest value in one flip,
`tail` the work after the last flip (in `total`), `per-flip` at most 65,536
sets. Each set has:

| Counter | Counts |
|---|---|
| `begin-end` | Begin/end pairs (the pusher's squash of repeated BEGIN, DRAW_ARRAYS, END counts each) |
| `vertices` | All vertices, split into `vertices-draw-arrays` (DRAW_ARRAYS counts), `vertices-inline-array` (INLINE_ARRAY words × 4 over the enabled attributes' vertex size), `vertices-array-element16` (two indices per word), `vertices-array-element32` and `vertices-immediate` (SET_VERTEX3F/4F, SET_VERTEX_DATA*) |
| `prim-*` | From each pair's vertex count n: `prim-points` (n), `prim-lines` (n/2), `prim-line-loop` (n segments), `prim-line-strip` (n-1), `prim-triangles` (n/3), `prim-triangle-strip` and `prim-triangle-fan` (n-2), `prim-quads` (n/4), `prim-quad-strip` ((n-2)/2), `prim-polygon` (1) |
| `triangles` | Triangles all primitives make (two per quad, n-2 per polygon) |
| `pushbuffer-bytes` | Pushbuffer words the pusher consumed (headers, data, jumps) × 4 |
| `vertex-bytes` | Per draw and enabled array attribute, stride × (last − first vertex) + element size (index and inline data come in the pushbuffer) |
| `texture-binds`, `texture-bytes-bound` | Enabled texture stages per pair and their sizes (all levels, cube faces, palette) |
| `texture-bytes`, `-uploaded`, `-new` | Distinct textures (address, size, format, palette) bound in the flip, each once; those whose guest bytes (hashed at first bind in the flip) changed since their previous bind in this counting run, what any renderer must upload again; those first bound in this counting run |
| `surface-bytes` | Distinct render targets drawn to in the flip (color with a color write mask bit set, zeta with depth or stencil writes on), each once, pitch × height with anti-aliasing |
| `clears`, `clear-pixels`, `clear-bytes` | Clears; the clear rectangle clamped to the surface, AA samples included; pixels × bytes per pixel |
| `reports`, `report-bytes`, `blits`, `blit-pixels` | GET_REPORT (16 bytes, 4 per semaphore release); NV_IMAGE_BLIT |
| `read-bytes-low`/`-high`, `write-bytes-low`/`-high` | Reads (pushbuffer, vertex and texture bytes, blit sources) and writes (surface, clear and report bytes, blit destinations), split at 64 MiB physical, plus `-high` per source, from the DMA objects' addresses as the renderers compute them |
| `occlusion-samples` | zpass pixel counts in reports that reached guest RAM: 0 under null; under Vulkan a fill estimate that depends on the driver and surface scale; under `XEMU_RENDER_NULL_RAM` the measured counts, while RAM gets 0 |

All but `occlusion-samples` are counted in PGRAPH and the pusher, so null
and Vulkan count the same command stream alike; where a game reads zpass
reports Vulkan can draw differently, unless `XEMU_RENDER_NULL_RAM=1`. Under
icount, counting started and stopped while paused is the same in every
process. Counting writes nothing the guest sees; off, it costs one predicted
branch per command.

## Guest RAM above 128 MiB

### Guest RAM

`[sys] mem_limit` (Settings > System > System Memory) is `'64'` (the
default) or `'128'`, plus experimental `'256'` and `'512'`. `-m` overrides
it for one run (the last `-m` wins); `xemu-ram-digest`'s `ram-size` reports
the result. Stock kernels manage 64 MiB at any size; a 128 MiB machine just
leaves the upper half to the BIOS memory test. A savestate loads only
into the size it was saved with ([Savestates and RAM
size](#savestates-and-ram-size)).

### More than 128 MiB

Experimental. `mem_limit` `'256'` or `'512'`, or `-m` above 128 in multiples
of 16 MiB up to 3840 MiB (where NV2A BAR1 and the PCI devices start; 192,
256, 512, 1024 and 3840 tested), gives the CPU that much RAM from physical
0. Other sizes exit 1 with `-m N MiB: guest RAM above 128 MiB must be a
multiple of 16 MiB, 3840 MiB at most`. stderr:
`xemu: guest RAM N MiB (experimental above 128 MiB): ...`.

- It helps only a kernel or mod that uses it. To the guest the machine looks
  like 128 MiB hardware: BAR1, the VGA window and `NV_PFB_CSTATUS` cover the
  first 128 MiB (`XEMU_CSTATUS_FULL_RAM=1` makes `NV_PFB_CSTATUS` report the
  full size).
- The X-codes program the NV2A through BAR0 at `0x0F000000`, inside RAM
  above 240 MiB, so BAR0 is mapped over that RAM until they move it.
- The harness's 4627 flash (`Complex_4627v1.03.bin`, 1de4c87effe4)
  cold-boots Azurik to its title at every tested size. The Vulkan renderer's
  vertex mirror is as big as guest RAM (host memory, touched only where
  vertices are).

### NV2A reads above 128 MiB

Experimental. At 256 MiB or more the NV2A masks DMA object bases, the PCRTC
start and 2D blit offsets to 28 bits instead of 27, so a DMA object can sit
in RAM's second 128 MiB (real hardware can't). stderr:
`xemu: NV2A wide addressing: 28 bits (guest RAM 256 MiB or more)`. Offsets
added to a base were never cut to 27 bits, so a base-0 object reaches past
128 MiB at any size. The mask stays 28 bits above 256 MiB and 27 below it;
BAR1, the VGA window (which wraps a scanout above 128 MiB) and
`NV_PFB_CSTATUS` stay 128 MiB. Stock kernels' DMA objects lie below 64 MiB
and run as at 27 bits. The mask follows the RAM size alone, so savestates
load across releases; v0.16.1 and older, and states shrunk into 144-240 MiB,
run at 27 bits: a DMA base above 128 MiB then wraps for textures, vertices,
the pushbuffer and 2D blits, and aborts the process as a render target.

## PAL 50 Hz

`[sys] vblank_rate` (Settings > System > Guest Vblank) sets the guest
vblank:

| `vblank_rate` | Guest vblank |
|---|---|
| `'60hz'` (default) | 60 Hz (16,666,666 ns), for every disc and EEPROM |
| `'eeprom'` | 50 Hz (20,000,000 ns) while the EEPROM is a PAL50 console's: video standard PAL-I (`0x00800300`) without the PAL-60 flag (`0x00400000` in the video flags at `0x94`); 60 Hz otherwise |
| `'50hz'` | 50 Hz always |

- PAL titles then run at PAL speed (Azurik Europe on a PAL50 EEPROM: about
  25 player ticks per guest second, not 30). xemu generates NTSC-M EEPROMs,
  so `'eeprom'` changes nothing without a PAL EEPROM.
- The rate is latched at machine start, every reset and every load
  (`loadvm`, `xemu-checkpoint-restore`, incoming migration), never mid-run,
  so replays stay exact. Snapshots hold the EEPROM, so a PAL50 state runs at
  50 Hz under `'eeprom'` whatever EEPROM the launch named; a guest that
  rewrites its EEPROM's video standard gets the new rate at its next reboot.
- Everything that counts frames counts these vblanks (`xemu-frame-advance`,
  `+N` input, live-pad sampling, value stops). Convert frames to guest
  seconds with `vblank-ns` (in `query-xemu-frame` and the
  `xemu-frame-advance` reply), not by assuming 60.
- stderr says `xemu: vblank 50 Hz (sys.vblank_rate ...)` at launch whenever
  the key isn't `'60hz'`, and again with `at load` or `at reset` whenever
  the rate changes (a Settings change takes effect at the next reset or
  load).

## QMP events and failures

### Events

QMP sends events only after `qmp_capabilities`. The matching stderr lines
stay.

| Event | Data | When |
|---|---|---|
| `XEMU_READY` | | Once per launch, with `xemu: ready` |
| `XEMU_STATE_LOADED` | `source, name, frame, loads` | Each successful load, with `xemu: state loaded` |
| `XEMU_DEVICE_LOST` | `reason, where, vk-result` | At most once, just before xemu dies of a lost Vulkan device |
| `XEMU_PRESENT_STALL` | `waited-ms, count` | Windowed Metal only, once per stall |
| `XEMU_CPUID_FAULT` | `pc, feature, opcode, count` | [Strict CPUID](#strict-cpuid) |
| `XEMU_GUEST_BUGCHECK` | `code, params, caller, count, regs, stack` | [Guest bugchecks](#guest-bugchecks) |
| `XEMU_GUEST_HALTED` | `eip, esp, count` | [Guest bugchecks](#guest-bugchecks) |

- **`XEMU_READY`** can go out before the client negotiates: after
  `qmp_capabilities`, call `query-xemu-status` (`{ready, loads}`) and wait
  for the event only while `ready` is false.
- **`XEMU_STATE_LOADED`**: `source` is `loadvm` (a disk-image snapshot;
  `name` is its tag), `checkpoint` (`name` is the checkpoint) or `incoming`
  (no `name`). `frame` is the frame counter at the load; `loads` counts this
  process's successful loads, this one included. It arrives before the reply
  of the `loadvm` (or `human-monitor-command`) or `xemu-checkpoint-restore`
  that caused it; after `migrate-incoming`, which replies first, it signals
  the load is done. Failed loads send nothing.
- **`XEMU_DEVICE_LOST`**: `reason` is `device-lost` or `fence-timeout` (no
  fence within 60 s, after a `still waiting` line at 5 s; headless then
  exits as for a failed call); `where` names the fence wait, if one saw it.
  Best effort: lost if the client left earlier output unread. What xemu
  prints and does is under [Lost GPU device](#lost-gpu-device).
- **`XEMU_PRESENT_STALL`** (0.16.1+): a present waited past
  `XEMU_PRESENT_STALL_MS` (default 100 ms) for a CAMetalLayer drawable. The
  window drops frames until one comes, printing `xemu: present stall N: no
  drawable for M ms; dropping frames until one comes` and
  `xemu: present stall N over after M ms, F frames dropped`; emulation,
  input and `screendump` go on.

### Lost GPU device

A lost Vulkan device makes a headless xemu print
`xemu: Vulkan device lost; exiting` and exit 1 at once; any other failed
Vulkan call prints `vk_result = N in <function> (<file>:<line>)`, then
headless exits 1 after `xemu: Vulkan call failed; exiting`. Windowed xemu
aborts in both cases. GPU errors are logged as they happen (MoltenVK
warnings, `xemu: Metal command buffer "..." failed on the GPU`).
`[display] renderer = 'OPENGL'` avoids MoltenVK and works offscreen, about
3x slower on Azurik's heavy scene.
