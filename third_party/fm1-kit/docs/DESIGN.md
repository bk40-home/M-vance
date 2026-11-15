# fm1-kit: one screen kit and one patch library for every firmware

**Proposal, 2026-11-14.** Status: signed off as recommended (Kris, 14 Nov). Built: the library (L0, L1) for
M-Vsynth2, at `third_party/fm1-kit/` until it is its own repo (the handover, section 1b). Next: K0, then M-vance. Covers M-Vsynth2 and M-vance first, and any later
firmware on the same platform (Dinghy / FM-1, SMK-37).

Mockup: the Design canvas "M-Vsynth2 screen UI: knobs, graphs and themes", artboard **Patch library: sources above,
FM-1 below**, in the SEA theme.

## 0. What I need from you

| | Decision | a (recommended) | b |
| --- | --- | --- | --- |
| 1 | Where shared code lives (2) | A new repo `fm1-kit`, a git submodule in each firmware at `third_party/fm1-kit`, pinned | Copied into each repo by a sync script, with a version file |
| 2 | What the screen kit covers (3) | The whole frame: themes, fonts, primitives, knob cell, header, footer, bands; graphs stay each app's, with shared plotting helpers | Themes and fonts only |
| 3 | The wire (4.6) | Each firmware keeps its own SysEx (M-vance must stay JP-8080's); the library reaches it through an adapter | A new common envelope for all |
| 4 | The library's memory (4.4) | The browser keeps the patches you open (IndexedDB, this computer only), exportable as a zip with a `library.json` | Nothing kept: files each time |
| 5 | Categories and tags (4.3) | A fixed category list plus free tags, kept in the library, not on the FM-1 (the FM-1 keeps colour only) | Category on the FM-1 too (record bytes; M-Vsynth2's record is full) |
| 6 | Dropping many patches on a slot (4.2) | Fill from that slot on, replacing, with the target shown before you let go, and undo; a "fill empty slots only" switch | Empty slots only, always |
| 7 | One page or one each (4.1) | **Decided (Kris, 14 Nov): one page a firmware**, each hosted on its own site (4.7) | |
| 8 | Order (6) | Kit extraction first (no visual change), then M-vance on the kit, then the library | Library first |

## 1. Why

Two firmwares already draw the same things two ways and load patches two ways (`docs/bank.html` in each). A third
would make three. The aim: the same look (the knob, the bands, the six themes) and the same library page on every
firmware, so a fix or a feature lands once.

## 2. Where it lives (decision 1)

```
fm1-kit/                         new repo, GPL-3.0-only
  screen/  kit_ui.h  kit_ui.c    themes, primitives, fonts, knob cell, bands, readout
           gen/kit_ui_gen.h      from tools/gen_kit_ui.py (fonts, knob tables, sine, exp)
  library/ core.js  ui.css       the library page's engine and look
           adapters/README.md    the adapter contract (4.6)
           test/                 node tests: a mock device, round trips
  tools/   gen_kit_ui.py  build_library.py
  docs/    DESIGN.md (sections 3 to 5 of this file, kept current)
```

Each firmware pins it as a submodule (as M-Vsynth2 already pins Synth2) and keeps only what is its own:
`xx_ui.c` (its pages to the kit's knobs, its graphs), `library/adapter-xx.js` (formats, wire, bank geometry).

## 3. The screen kit (decision 2)

Taken out of M-Vsynth2's `vs_ui.c`, renamed `kit_`, with no M-Vsynth2 names left in it.

| In the kit | Stays in each app |
| --- | --- |
| Themes (SEA, EMBER, ICE, ORCHID, CRT, PAPER), tokens, `mix565` | Which settings bytes hold THEME and COLOUR |
| Fonts `FONT_LAB`, `FONT_VAL`, `FONT_TTL`, `FONT_BIG` | |
| Primitives: `rrect`, `disc`, `seg`, `vspan`, `curve`, `under`, `txt`, `txt_fit`, `chip` | |
| Bands: header (chip, title, pips, voice cells), panel in strips, cell, footer; the hash-a-band redraw | What the header and footer say |
| The knob cell: VALUE, CENTRE, CHOICE, SWITCH, ROUTE, READ, in hand, modulation arc | Mapping a parameter to a knob (`kit_knob_t`) |
| The readout of the knob in hand | |
| Plot helpers: grid, curve with fill, envelope (ADSR), filter response, LFO waves, a wave | Which graph a page shows; graphs only it has (M-vance's reverb, Synth2's phasers) |
| The scope's drawing | The ISR copy (it lives where `app_audio` is) |

The app gives the kit a small table of callbacks: the voices' states, the parameter's text, the knob for a parameter.
Rule: the kit never reads an engine. Tests: the host builds of both firmwares, a screenshot script each, and a pixel
compare against the screenshots taken before the extraction (identical for M-Vsynth2).

## 4. The patch library page

Best of what the field does, put together:

| Seen in | Taken |
| --- | --- |
| SHRUB's editor | The bank as a grid of 8 rows of 16 coloured slots; drag slots to reorder, Alt-drag to copy; drag in from factory lists or the desktop; meters for slots used |
| Novation Components | A pack: open a source on the left, drag one or many into slots; drag files straight from the file manager; a backup of what was on the device before a send |
| u-he and NI browsers | Categories you can click straight away, sub-tags optional, free tags |
| knob.monster | Browser only, no install; audition a patch on the hardware with one click; a sandbox with no device |
| KnobKraft Orm | One library across many synths, with user banks |

### 4.1 Layout (decision 7)

Two banks, one above the other, as the Access Virus TI editor's librarian does: what you have opened on top, the FM-1
below, so a drag always goes down (the mockup, revised 14 Nov after Kris's review).

- **Full names, always** (Kris, 14 Nov). A name is never cut: the page measures the longest name in the bank (the
  formats allow 16 characters; check M-vance's) and sizes the columns to it, then fits as many columns as the window
  holds. At 1440 px that is 8 columns of 16 rows for typical names; a bank of wide names becomes 7 or 6 columns with
  more rows, and the pane scrolls rather than truncating. The full name is also the slot's tooltip and accessible name.
- **Top, the source banks**: a tab for each bank opened (a factory bank, a zip bank, a folder, a `.syx` bank), and
  "All sources". A bank shows all its slots at once, numbered down the columns (001..016 first), each row 20 px with
  its number, a category stripe and the name. Search and category chips (in the top bar) dim what does not match
  rather than reflowing the bank, so slot positions stay put; "All sources" lists the matches only. Shift / Ctrl
  select many.
- **Bottom, the FM-1's banks**: the same view, the bank tabs (FACTORY, U1..U8, or the firmware's own) with how many
  slots each uses, Read, Download, **Send n changes**, "Empty slots only". Empty slots show a dash; a changed, unsent
  slot a dot. The slots a drop will fill are outlined before you let go.
- **A splitter** between the two: drag it to give one bank more room.
- **Patch details** open on demand (the I key, a double-click, or the button in the top bar), as a panel over the
  right edge: name, source and file, Audition, Rename, category and colour, tags, small filter and envelope drawings
  in the FM-1's colours, and "also in U1 041" for duplicates. Kept off the main view so the banks get the full width.
- **Top bar**: search across every source, category chips, Patch details, the theme (the same six names).
- **Foot**: the transfer, write by write, with each acknowledgement; the keyboard keys.

At 1440 x 900 this shows 256 patches at once, both banks whole, every name in full.

### 4.2 Drag and drop (decision 6)

| Gesture | Does |
| --- | --- |
| One patch onto a slot | Puts it there (replacing, the slot marked changed) |
| Many onto a slot | Fills from that slot on, in the list's order; the slots it will fill are outlined before you let go, with "replaces n"; past slot 128 it stops and says how many did not fit |
| With "fill empty slots only" | Skips filled slots instead |
| Files from the desktop onto a slot | Opened, then dropped as above |
| A slot onto a slot | Moves it (swaps when the target is filled); Alt copies |
| A slot onto the left list | Takes it into the library |
| Keyboard | Arrow keys move, Space selects, `S` sends the selection from the selected slot on, Delete clears a slot; every drag has a button too |

Nothing reaches the FM-1 until **Send**: it writes only the changed slots, then reads them back and compares. Undo
(Ctrl+Z) works on the page's own state. Before a send, the page keeps the bank as it was (Components' backup idea),
one click to put it back.

### 4.3 Search, categories, tags (decision 5)

- Instant search over name, tags, category, source. Filters typed or clicked: `cat:pad`, `colour:blue`,
  `src:factory`, `tag:warm`, `bank:U1`, `dup`.
- Categories (one each): BASS, LEAD, PAD, KEYS, PLUCK, BRASS, STRINGS, FX, SEQ, DRUM, OTHER. A first guess from the name
  (`PAD Glass` is a pad), always editable.
- Tags: free, many, shown as chips.
- The FM-1 keeps the colour only (M-Vsynth2's record is 120 bytes, full). Category and tags travel in the library's
  zip as `library.json` beside the `.sy1` files, which stay as Synth1 writes them.

### 4.4 What the page keeps (decision 4)

IndexedDB in this browser: the patches opened, their tags, the last bank read. Export and import as a zip (the
patches as their own files, plus `library.json`). Nothing goes to a server.

### 4.5 Look

The device's tokens as CSS variables, the same six themes, Inter (or DejaVu Sans Condensed to match the screen).
Accessible as drawn: real buttons and inputs, labels, text contrast at 4.5:1 or more, focus rings, every drag
has a keyboard way.

### 4.6 The adapter contract (decision 3)

```js
export default {
  id: "mvsynth2",                         // matches the firmware's identity
  name: "M-Vsynth2",
  banks: [{id: 0, name: "FACTORY", slots: 128, readOnly: true}, /* U1..U8 */],
  files: [".sy1", ".zip"],                // what Open accepts
  parse(file) -> [patch],                 // to the page's patch: {name, colour, values, raw, source}
  serialise(patch) -> {name, bytes},      // to a file the synth itself opens (.sy1 ver=112)
  identity(midi) -> {firmware, version},
  read(midi, bank) -> [patch|null],       // the bank, slot by slot
  write(midi, bank, slot, patch) -> ack,  // acknowledged write
  clear(midi, bank, slot),
  audition(midi, patch),                  // plays it, nothing saved
  preview(patch) -> {filter, env, ...},   // values for the inspector's drawings
}
```

M-Vsynth2's adapter wraps today's `bank.html` logic (`tools/sy1.py`'s rules in JS, the `7D` SysEx). M-vance's wraps its
JP-8080 SysEx. A mock device (in `library/test/`) speaks each protocol, so the page works with no FM-1 (knob.monster's
sandbox) and the tests run in Node.

### 4.7 Hosting: a page a firmware

Each firmware's page is built into **one self-contained `library.html`** (`tools/build_library.py` inlines the kit's
core, its CSS and that firmware's adapter, with the kit's version and the firmware's in the page), committed in the
firmware's repo under `docs/` and hosted with its own site, beside its README and cheat sheet. Nothing is loaded from
another site, so a page keeps working wherever it is hosted, and offline.

The page asks the device who it is on connect. A different firmware (M-vance's page meeting M-Vsynth2) is refused
with its name and a link to its own page, never written to. The theme and the library in IndexedDB are kept per page
(each site has its own storage), so two firmwares' libraries never mix.

## 5. Not in this proposal

A cloud library, sharing between users, editing sounds in the page (Shrub's editor does; ours would be a later
step), iOS (no Web MIDI there).

## 6. Stages (decision 8)

| Stage | What | Test on the computer | Check on the FM-1 |
| --- | --- | --- | --- |
| K0 | `fm1-kit` repo, the screen kit taken out of `vs_ui.c`; M-Vsynth2 on the submodule | Host screenshots identical to before, pixel for pixel; `run_unit.sh` | Unchanged look |
| K1 | M-vance on the kit: its pages to kit knobs, its graphs | M-vance's host screenshots, every page, every theme | Every page by hand, CPU unchanged |
| L0 | Library core, M-Vsynth2 adapter, parity with today's `bank.html` (open, play, send, read, download) | Node tests: parse and serialise every factory `.sy1` byte for byte; mock device round trip | A bank sent and read back |
| L1 | Search, categories, tags, multi-drop, move and copy, undo, send only changes, the backup | Node tests on the drop rules; a scripted UI test in Chrome | 40 patches dropped and sent |
| L2 | M-vance adapter | Its formats and SysEx against the mock | A bank sent and read back |
| L3 | Docs: `DESIGN.md`; each firmware's README and cheat sheet point to its own hosted page | `run_docs.sh`; `build_library.py` checks the page has no outside URL | Each page from its host |
