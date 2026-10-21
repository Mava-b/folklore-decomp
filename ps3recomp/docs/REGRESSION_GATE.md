# The regression gate

Most of the ports are not being actively worked on. That is exactly why they are
useful: without something watching them, a fix for one game silently breaks
another and nobody finds out for weeks. The gate is what makes it safe to change
shared runtime code.

```bash
python tools/regress.py list                 # registered ports and golden status
python tools/regress.py check --all          # every port, against its golden
python tools/regress.py check vf5            # one port
python tools/regress.py record vf5           # capture a new golden
python tools/regress.py check --all --shots  # render profile: keep the frames
```

A full run builds the toolkit, rebuilds each port **against that toolkit**, runs
it, and diffs. Roughly 25 minutes for five ports.

## What it compares

An always-on, cheap event stream emitted by the *shared runtime*, so every port
gets it with no per-port code: the ordered first occurrence of every firmware
import, every lv2 syscall and every RSX draw path, plus occurrence counts and a
few scalars. See `include/ps3emu/milestone.h` for why each property is there.

It is deliberately **not** screenshots (needs a GPU, brittle to legitimate
rendering changes) and **not** the verbose trace (verbose logging starves guest
threads and changes what a title does, so a gate built on it would lie about the
thing it exists to check).

| verdict | meaning |
|---|---|
| `LOST` | a call the title used to make and no longer does. Fails. |
| `UNRESOLVED` | a firmware import that has *become* unresolved. Fails. |
| `DROPPED` | an occurrence count that fell a whole decade, or a scalar that went down. Rendering stopping looks like this. Fails. |
| `RESOLVED` | an `unresolved:*` key disappeared — the import gained a handler. Passes. |
| `NEW` / `RAISED` / `ORDER` | reported, does not fail. As likely progress as regression, and a gate that cries wolf gets ignored. |

## Reading a result honestly

This is the part that costs people time, so it is worth stating plainly: **most
reds are not regressions.** In one session five were investigated and four were
measurement artefacts. The gate now defends against each, but the habits still
matter.

**A run straight after a build is not a measurement.** Cold page cache for a
20–40 MB executable and its assets shifts guest timing enough to flip
race-sensitive titles. That run is discarded as a warm-up; the measurement is the
run after it. VF5 once reported 11 regressions on a tree where four warm runs
were clean, and YDKJ went 120 milestone keys to 80 across three fresh builds
while three warm re-runs of the *same binary* gave 120 and a pass.

**A second run catches an independent flake, not a correlated one.** Confirming a
red by re-running helps when the flake is random. It does not help when the cause
is the post-build state itself, because both runs sit inside it and agree on a
wrong answer. That is why the warm-up exists as well as the confirmation.

**Check the port is configured the way its own README documents.** `PS3_VFS_ROOT`
is the host directory `/dev_bdvd` maps to and the guest path is appended *whole*,
so a root of `vfs/PS3_GAME/USRDIR` against a guest path already containing
`PS3_GAME/USRDIR` resolves to a directory that exists nowhere. VF5 spun on
686,996 failed opens and looked like a boot regression; Simpsons opened zero
files and drew 9,391 groups of nothing, which looked like a renderer failure.
Neither was.

**Facts about the build are trustworthy; facts about a run need a second run.**
An `UNRESOLVED` NID is a property of the binary and can be believed immediately.
A key count is a property of one execution.

**A golden records one run, so it bakes in that run's races.** Two of the first
six were recorded from a minority outcome and failed on unmodified master.
Recording twice and keeping the intersection would have caught both.

## `--shots`: does it actually render?

The default run is headless on the null backend — fast, GPU-free, identical on
Linux and macOS. It therefore has no opinion on whether a title *renders*, only
on whether it submits draw methods. For VF5 it does not even have that: its draws
exist only on the live-draw path, which the headless run disables.

`--shots` runs each port in the configuration its own README documents
(`render_env` in the registry) and keeps the presented frames. Frames are judged
by **distinct colour count**, never by brightness: a title presenting a cleared
buffer fills the screen with one flat colour and scores 100% non-black while
showing nothing.

`render_gold` points at the port's own reference screenshot, tracked in its repo
— the picture someone kept because the title looked right that day. `--shots`
writes a side-by-side `<port>-vs-gold.png`. There is no pixel diff: the
references carry window chrome at assorted sizes and attract modes animate, so a
strict comparison would only ever cry wolf.

`--shots` needs a GPU, so it is opt-in and not what CI runs. Its milestone stream
is a different configuration and is deliberately not diffed against the headless
golden.

## The registry

`tools/regress_ports.toml`. Each port names its directory, command, timeout,
environment, what finishing looks like (`expect`), and optionally a
`render_env` / `render_gold` / `render_timeout` for `--shots`.

A port whose directory or executable is missing is **SKIPPED**, not failed —
game binaries are never in this repo, and the gate has to be useful to someone
who owns two of the titles.

`expect` says what success looks like: `"timeout"` (reached a frame loop and
never exits — the usual case), `"exit0"`, or `"any"` (the terminal state is not
stable enough to assert; compare the milestones only). Tokyo Jungle and You Don't
Know Jack both use `"any"`: YDKJ reaches the same 118 keys ending at
`rsx:draw_arrays` whether it survives to the timeout or exits `0xC0000005`.

Two titles are deliberately absent. **flOw**'s lifted tree is git-ignored and
cannot be regenerated, so a golden from it could never be reproduced.
**Rubber Ducky** is a Sony SDK sample recompiled from a debug fSELF with its
symbol map exposed — nothing else is built that way and no retail title will be,
so it says nothing about whether a change is safe for real games. It remains
valuable as ground truth for the lifter, which is a different job.

## Known gap: the gate is not hermetic

A run mutates the port's on-disk state. Twisted Metal's first run creates its
game data, so the *next* run sees the data as installed and never calls
`cellGameCreateGameData` at all. The install and first-boot path — the one the
`cellGame` work is about — is therefore only exercised once per machine unless
the directory is deleted by hand.
