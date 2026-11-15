# fm1-kit

The parts every FM-1 firmware of Kris's shares: one patch library page, the web look, a firmware page and (next) one
screen kit. GPL-3.0-only.

**Status (14 Nov 2026):** the library is here; the screen kit is still `firmware/src/vs_ui.c` in M-Vsynth2 (stage K0
of `docs/DESIGN.md` takes it out). This folder lives at `third_party/fm1-kit/` in M-Vsynth2 and is meant to become its
own repo, pinned as a git submodule at the same path in each firmware:

```sh
cd third_party/fm1-kit && git init && git add . && git commit -m "fm1-kit 0.1.0: the library page"
# push it as github.com/<you>/fm1-kit, then in each firmware:
git rm -r --cached third_party/fm1-kit && git submodule add <url> third_party/fm1-kit
```

## The firmware page (0.2.0)

```
web/fm1ota.js          the install over Web MIDI: tools/fm1_install.py (Felucca's) line for line, as an ES module
web/flash.js           the page: find the FM-1, the latest release or a .fwsc, install, progress, what to do on failure
web/flash.css          its look, on theme.css
web/count.js           the page counter (GoatCounter, no cookies): views and fm1Count("event"); off unless the
                       <meta name="fm1-count"> holds the counter's address, never on localhost or under DNT / GPC
web/test/              node web/test/fm1ota_test.mjs [FIRMWARE_REPO]: a pretend FM-1; with the repo, the same answers
                       as its tools/fm1_install.py on its packages
```

A firmware copies the web files into its `docs/flash/` with its own `index.html` (M-Vsynth2: `flash/`), and its Pages
workflow puts the latest release's packages and `firmware/latest.json` beside them. The library page inlines
`count.js` too.

## The library page

```
library/shell.html     the page's markup          library/ui.css   the look: the screen's six themes
library/model.js       drops, moves, undo, search, categories (pure; tested in Node)
library/zip.js         zip in (stored, deflate) and out (stored), no library
library/core.js        the page: sources above, the device below, drag one or many, send, read back
library/adapters/      what a firmware gives the page (README.md)
library/test/          node library/test/model_test.js
tools/build_library.py one self-contained HTML file a firmware: nothing loaded from outside
```

A firmware has `library/adapter-<id>.js` and `library/mock-<id>.js` and a generator that calls `build()`; its page
goes in its own `docs/` and is hosted with it. Each page refuses a device running another firmware.

What it does: open `.sy1` files, folders, zip banks or an exported library (they are kept in this browser); search
(`cat:pad tag:warm colour:blue src:factory bank:u1 dup`) and category chips; banks shown whole, names never cut;
drag one patch or many onto a slot (they fill from there; "Empty slots only" skips filled ones), move and Alt-copy
inside a bank, take patches from the device into the library; undo; Send writes only what changed, each write
acknowledged and read back; Restore puts a bank back as it was before the session's first send; audition; rename,
colour, category, tags; download a bank or the whole library as zip; a Sandbox device to try it all with nothing
plugged in; keyboard for everything.

## Changes

- **0.2.0 (15 Nov 2026, made in M-vance's and M-Vsynth2's copies, merged here):**
  all additive, M-Vsynth2's page unchanged unless it opts in.
  - **AMBER theme:** M-vance's screen orange, in `library/ui.css`, `web/theme.css` and `web/theme.js`.
  - **`adapter.theme`:** the theme a first visit shows.
  - **`adapter.actions`:** buttons beside Audition, with `FK.lib.putAt(ref, patch)` to put a patch back in a slot later (M-vance's Edit → Editor → Keep).
  - **`adapter.bankFile(slots)`:** Download writes the bank as one file (M-vance: one `.syx`, as `make_bank.py` writes it).
  - **No `device.clear`:** a device whose slots are never empty; Del is refused with a reason.
  - **One colour:** the details panel hides the colour row.
  - **The firmware page** (made in M-Vsynth2's copy): `web/fm1ota.js` (fm1_install.py as an ES module), `web/flash.js`,
    `web/flash.css`, `web/test/fm1ota_test.mjs`.
  - **The page counter:** `web/count.js` (GoatCounter, no cookies), inlined in the library page by `build_library.py`
    (`library/shell.html` has the `fm1-count` meta: off until the host fills it).
