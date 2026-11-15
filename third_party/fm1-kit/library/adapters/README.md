# The adapter: what a firmware gives the library page

The page (`library/core.js`) knows nothing about any firmware. A firmware's adapter, one or two plain scripts in that
firmware's repo, sets `FK.adapter` before `core.js` runs, and `FK.mockDevice` for the Sandbox. M-Vsynth2's
(`library/adapter-mvsynth2.js`, `library/mock-mvsynth2.js`) is the worked example.

## `FK.adapter`

| Field | What |
| --- | --- |
| `id` | Short name: the IndexedDB and localStorage keys, the zip names (`"mvsynth2"`) |
| `name` | Shown in the top bar and the messages (`"M-Vsynth2"`) |
| `product` | A RegExp the device's identity must match, or the page refuses it (`/^M-VSYNTH2$/`) |
| `deviceName` | `"FM-1"` |
| `portMatch` | A RegExp to pick the MIDI port by name |
| `nameLength` | A patch name's most characters (the grids are sized to show them whole) |
| `bankSize` | Slots in a source bank (a folder of more becomes more banks) |
| `deviceBanks` | `[{id, name, size, readOnly}]`, in tab order; the first not read-only is shown first |
| `fileTypes` | What Open accepts besides `.zip` (`[".sy1"]`) |
| `colours` | The patch colours, by name; each needs a `--pc-<name>` in `ui.css` (or the adapter's own CSS) |
| `nameOf(p)`, `colourOf(p)` | A patch's name (no padding) and colour name |
| `withName(p, s)`, `withColour(p, c)` | A copy with that name or colour (patches are never changed in place) |
| `fingerprint(p)` | A string: equal for identical patches (name, colour, values). Equal means "no change to send" |
| `soundKey(p)` | A string: equal for the same sound whatever its name (finds duplicates) |
| `parseFile(name, bytes)` | `[patch, ...]` from one file (a bank file may hold many), or `null` if not a patch file |
| `fileName(p, slot)` | The file a slot is written as in a zip (`"012.sy1"`) |
| `serialise(p)` | `Uint8Array`: the file the synth itself opens |
| `parseFile` holes | (0.3.0) A sparse array keeps each patch at its index (`out[slot] = p`): a bank file that names its slots reopens with them where they were; holes are empty slots |
| `bankFile(slots)` | Optional (0.2.0): `{name, bytes, note?}`, the bank as one file for Save… (else a zip of `serialise`d patches); `slots` may hold nulls (0.3.0: a bank of your own with gaps); `note` is added to the status line; null to fall back |
| `describe(p)` | Optional: a short line for the details panel |
| `preview(p)` | `{filter: {label, kind: "lp"/"hp"/"bp", order: 2/4, freq: 0..1, res: 0..1}, env: {a, d, s, r: 0..1}, facts: [[label, value]]}` |
| `factory()` | `[{name, slots}]`: banks built into the page (shown as read-only sources) |
| `device.identify(io)` | `{product, version}` |
| `device.read(io, bank, size, onProgress)` | The bank's slots: patch or `null` |
| `device.readSlot(io, bank, slot)` | One slot (the read-back after a send) |
| `device.write(io, bank, slot, p)`, `device.clear(io, bank, slot)` | Resolve when the device has it; throw with a reason. No `clear` (0.2.0): the device's slots are never empty, and Del is refused |
| `device.audition(io, p)` | Plays it now, saves nothing |
| `mock()` | A pretend device: an `io` |
| `theme` | Optional (0.2.0): the theme a first visit shows (`"amber"`); else SEA |
| `layers` | Optional (0.3.0): a patch made of parts (M-vance's UPPER / LOWER). `{names: [2 names], modeLabel, modes: [labels], mode(p), withMode(p, m), params: [{label, min, max, show(p), get(p), set(p, v), fmt(v)}], heard(p, k), nameOf(p, k), take(p, k, from, fk), swap(p), copy(p, a, b), solo(p, k)}`: every function returns a new patch. The details panel shows the mode, each part's name, ▶ (`solo`, auditioned), a drop target (`take` with `fk` 0, Alt 1), Swap and Copy |
| `actions` | Optional (0.2.0): `[{label, title, run(p, ref, where)}]`, buttons beside Audition in the details panel. `ref` names the slot (`{dev: bank, s}` or `{src: id, s}`; null in All sources); `FK.lib.putAt(ref, p)` later puts a patch back there as an ordinary change (a device slot: pending until Send, undoable) and returns `""`, or the reason it could not |

## `io`

`{send(bytes), listen(fn) -> unsubscribe, close()}`. The page makes one from Web MIDI (SysEx on); the mock is one. An
adapter that has no acknowledgement on its wire (M-vance's JP-8080 SysEx) paces its writes inside `write`; the page
reads every sent slot back with `readSlot` and compares fingerprints either way.

## Data

The build script puts `window.FK_DATA` first: whatever the adapter needs (rule tables, factory files).
