# Mac: what isn't production-ready yet

The macOS version is the reference: its features are the ones in [`SPECIFICATION.md`](SPECIFICATION.md). This file lists
what the Mac version still lacks or gets wrong against that list and the project's rules, in build order, each with
its spec section. Fix broken items first, one pull request each, and tick an item with its PR number once it is built
and its test passes on the Mac. [`bugs.md`](bugs.md) and [`enhancements.md`](enhancements.md) stay the record of requests and
releases.

## Bugs to fix

No open bugs in `bugs.md`.

- [ ] **SSH questions default to Continue.** The "SSH host key changed" question and other SSH yes/no prompts make
  Continue the default button, so Return replaces a stored host key. They should default to Cancel, as on Windows and
  Linux. (§5 S2, S3)

## To build

- [ ] **Title bar reads "PTZ Control" only.** Drop the window's subtitle "PTZ Coordinates Controller by Herbert
  Cisneros". (§2 K1)
- [ ] **Hover text on every control**: what it is, what it does and its shortcut, for the route and vendor choices,
  every field, Connect, the Advanced triangle, Move, Return to Start, Stop, Web Interface, the plane and the meters.
  (§3, §7, §8, §10)
- [ ] **VoiceOver for the instruments.** The position plane and the three travel meters are drawn views with no
  accessibility label, value or actions, so VoiceOver can't read the position or aim with them. (§7 I3–I6)
- [ ] **Feature tests.** A test that drives the app's own commands against the mock camera and checks each feature's
  result, with the results per feature fed into this file. Today only the snapshot hook exists, and no script runs
  it or checks what it saved. (§16 T4)

## Not checked automatically

The core tests (§16 T2, T3) cover the core against the mock camera and a local `sshd`. Nothing checks the app
itself, so these are tried by hand before a release:

- [ ] Window, layout and closing while connected (§2 K1–K4).
- [ ] Link configuration, its checks, remembered settings and keyboard (§3 F1–F9).
- [ ] Connect, Cancel, Disconnect and link lost in the app (§4 L1–L5).
- [ ] SSH prompts as dialogs, the changed host key question and the cipher fallback (§5 S2–S4); the SSH test answers
  prompts in code, and never changes a host key or forces the fallback.
- [ ] A real ONVIF and a real Axis camera (§6 V1–V5); the tests use the mock camera only.
- [ ] Instruments, aiming and double-click moves (§7 I1–I8).
- [ ] Moves, Return to Start, Stop and queueing from the window (§8 M1–M5).
- [ ] Refresh, pause and pause after failures (§9 R1–R5).
- [ ] Opening the browser and the fallback dialog (§10 W1–W2).
- [ ] Event buffer (§11 E1–E2), phosphor themes and light/dark (§12 A1–A3), menus, help and About (§13 H1–H3).
