<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Sound match: a recorded note in, an M-vance patch out

You give it a WAV of **one held note**. It finds the M-vance (JP-8080) patch that sounds closest by repeatedly
playing patches on the real M-vance engine and comparing them with your sample. Then it writes a page where you
can listen to the two side by side.

## Once: install what it needs (WSL)

Open Ubuntu (WSL) and run:

```
python3 -m pip install --user --break-system-packages numpy scipy cma soundfile matplotlib
```

If it says `No module named pip`, first run `sudo apt update && sudo apt install -y python3-pip`. (Sound match
checks this when it starts and prints the same command if something is missing.)

## The easy way: in the browser

1. Double-click **`tools\match\start-match.bat`** (in File Explorer). A black window opens (leave it open) and
   your browser opens the sound-match page.
2. Drag a WAV onto the page and press **Run** (set the note too, if you know it).
3. Watch the progress bar. When it's done the page shows:
   - the score, and the report with players and pictures;
   - **Download &lt;sample name&gt;.syx**: the patch.

   Past runs are listed on the right (kept in `build\match\web\`).
4. Close the black window to stop it.

If the browser doesn't open by itself, go to **http://localhost:8765**.

## From the command line

1. Put your sample somewhere on the PC, for example `C:\GitHub\M-vance\samples\my_sound.wav`.
2. In WSL:

   ```
   cd /mnt/c/GitHub/M-vance
   python3 tools/match/match.py samples/my_sound.wav
   ```

   It takes a few minutes and prints its progress. At the end it prints a line starting **OPEN:**.
3. Open that `report.html` (paste the path into File Explorer or a browser). It has:
   - two players, your sample and the match, at the same loudness;
   - a score: **close**, **fair** or **poor**;
   - pictures comparing the two;
   - every knob the match set.
4. To hear it on the FM-1: on the page, press **Open in bank editor** (Chrome or Edge). The patch is already in the
   editor's palette: press **Connect FM-1**, then **▶** next to it. To keep it, drag it into a slot, tick the slot
   and **Send ticked**. (From the command line: open the bank editor, `docs/sim/bank/editor.html` or Bank editor on
   the M-vance site, and **Add source parts** with the `.syx`.)

A whole folder of WAVs works too: `python3 tools/match/match.py samples/`.

## Tips

- If you know the note you played, add it as a MIDI number: `--note 60` for middle C. **Ableton calls middle C
  "C3"**, so Ableton's C3 is `--note 60` (the page's list shows both names).
- If you know how long the key was held: `--gate-ms 2000`.
- The best sample is one note, held, then released, with its tail, and dry (no reverb).
- Sounds the synth can't make (piano, voice) get the nearest patch and a **poor** score.

## What's in the output folder (`build/match/out/<sample name>/`)

| File | What it is |
|---|---|
| `report.html` | **open this** |
| `<sample name>.syx` | the patch (JP-8080 user patch 1): open it in the bank editor; the JT-8000's importer reads it too |
| `match.wav`, `target.wav` | the match and your sample as sound files |
| `seed.json`, `log.txt` | what it read from the sample, and how the search went |

## For checking the tool itself (not needed to use it)

`serve.py` is the page's server (`python3 tools/match/serve.py`, options `--port`, `--workers`, `--no-open`); `match.html` is the page.


`test_wp0.py` .. `test_wp4.py` are the work-package acceptance tests (docs in the project's `sound-match/` folder).
