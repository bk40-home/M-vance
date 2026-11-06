#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Make the M-vance cheat sheet (docs/cheatsheet.html) from the panel itself.

  MV_DUMP_PAGES=build/pages.json build/host/mvance build/host/mvance     (tests/run_docs.sh does this)
  python3 tools/gen_cheatsheet.py build/pages.json docs/cheatsheet.html [VERSION]

What comes from the firmware (build/pages.json, written by mv_ui_dump in mv_ui.c): every page in panel order, the
button that reaches it, what KNOB 1-4 edit, their ranges, choice names, CCs and which settings are global. So the
sheet cannot drift from the panel. What is written here: one short line per knob (WHAT, below) and the "getting
around" notes. A knob with no line in WHAT stops the build, so a new page cannot go out undocumented.

Two A4 landscape pages, the same file on screen and on paper (print it at 100 %, or "Save as PDF"):
  1  the panel: the controls, then every button's pages with KNOB 1-4
  2  the lists: the choice names, MIDI, what is kept where
"""
import html
import json
import sys

# One line per knob: "PAGE TITLE|LABEL" -> what it does ("" when the label says it all). Every knob needs an entry,
# even an empty one: a missing entry stops the build. Keep each short: it shares a row with three others.
WHAT = {
    "LFO|WAVE": "",
    "LFO|RATE 1": "",
    "LFO|FADE": "LFO 1 fades in over this time",
    "LFO|DEST": "LFO1 & ENV pitch to",
    "LFO|RATE 2": "LFO 2 speed (mod wheel brings it in)",
    "LFO|DEPTH>": "LFO 2 depth select (stored only)",
    "LFO|P.LFO1": "LFO 1 to pitch (vibrato)",
    "LFO|P.LFO2": "LFO 2 to pitch, by mod wheel",
    "OSC 1|WAVE": "",
    "OSC 1|CTRL1": "control 1: its job follows the wave",
    "OSC 1|CTRL2": "control 2: its job follows the wave",
    "OSC 2|WAVE": "",
    "OSC 2|CTRL1": "control 1: its job follows the wave",
    "OSC 2|CTRL2": "control 2: its job follows the wave",
    "OSC 2|SYNC": "OSC 2 hard-synced to OSC 1",
    "LFO|SYNC 1": "LFO 1 to the tempo (OFF: RATE 1)",
    "OSC 2|RANGE": "OSC 2 pitch in semitones; ends: WIDE",
    "OSC 2|FINE": "fine tune (WIDE: big steps)",
    "OSC COMMON|RING": "ring modulator",
    "OSC COMMON|X-MOD": "cross modulation OSC 2 to OSC 1",
    "OSC COMMON|BALANCE": "OSC 1 / OSC 2 mix",
    "OSC COMMON|SHIFT": "both oscillators, in octaves",
    "PITCH ENV|DEPTH": "pitch envelope amount (+-63: 4 oct)",
    "PITCH ENV|ATTACK": "",
    "PITCH ENV|DECAY": "",
    "FILTER|TYPE": "",
    "FILTER|SLOPE": "dB per octave",
    "FILTER|CUTOFF": "",
    "FILTER|RESO": "",
    "FILTER|KEY FLW": "cutoff follows the keys",
    "FILTER|LFO1": "LFO 1 to cutoff",
    "FILTER|LFO2": "LFO 2 to cutoff, by mod wheel",
    "FILTER|ENV DEP": "filter envelope amount (+63: all the way)",
    "FILTER ENV|ATTACK": "",
    "FILTER ENV|DECAY": "",
    "FILTER ENV|SUSTAIN": "",
    "FILTER ENV|RELEASE": "",
    "AMP|LEVEL": "",
    "AMP|LFO1": "by PAN: tremolo, auto-pan, position",
    "AMP|LFO2": "LFO 2 tremolo, by mod wheel",
    "AMP|PAN": "what AMP LFO1 does",
    "AMP ENV|ATTACK": "",
    "AMP ENV|DECAY": "",
    "AMP ENV|SUSTAIN": "",
    "AMP ENV|RELEASE": "",
    "TONE|BASS": "",
    "TONE|TREBLE": "",
    "TONE|GAIN": "",
    "MULTI-FX|TYPE": "chorus, flanger, phaser .. DIST",
    "MULTI-FX|LEVEL": "effect amount (DIST: drive)",
    "DELAY|TYPE": "",
    "DELAY|TIME": "",
    "DELAY|FEEDBK": "",
    "DELAY|LEVEL": "",
    "REVERB|LEVEL": "reverb amount (0: off)",
    "REVERB|SIZE": "",
    "REVERB|HI DAMP": "darker tail",
    "REVERB|LO DAMP": "thinner tail",
    "REVERB|LOWPASS": "top cut of the reverb",
    "REVERB|HIPASS": "bottom cut of the reverb",
    "REVERB|FREEZE": "hold the tail forever",
    "PLAY MODE|MONO": "one voice, last note",
    "PLAY MODE|LEGATO": "MONO: no retrigger",
    "PLAY MODE|PORTA": "",
    "PLAY MODE|TIME": "",
    "PLAY MODE|UNISON": "all voices, one note",
    "PLAY MODE|DETUNE": "",
    "PLAY MODE|BEND +": "bend up range, semitones",
    "PLAY MODE|BEND -": "bend down range, semitones",
    "PERFORM|MODE": "SINGLE, DUAL (both parts), SPLIT",
    "PERFORM|SPLIT": "LOWER below it; touch, then press a key",
    "PERFORM|BALANCE": "LOWER .. UPPER, 64 both full",
    "PERFORM|PANEL": "the part the pages edit (header U/L)",
    "PERFORM|VOICES": "the 8 voices: LOWER + UPPER",
    "PERFORM|ARP": "the part(s) the arp plays",
    "PERFORM|CH LO": "LOWER's MIDI channel (0: GLOBAL)",
    "PERFORM|CH UP": "UPPER's MIDI channel (0: GLOBAL)",
    "GLOBAL|MIDI CH": "OMNI: in on all, out on 1",
    "GLOBAL|VOICES": "polyphony limit (CPU)",
    "GLOBAL|TEMPO": "arp BPM; a MIDI clock overrides it",
    "ARP|MODE": "",
    "ARP|RATE": "",
    "ARP|OCTAVE": "",
    "ARP|GATE": "note length within the step",
    "ARP|HOLD": "latch: notes go on after the keys",
    "ARP|SWING": "every second step later",
    "ARP|CHANCE": "probability a step plays",
    "ARP|ORDER": "by pitch or as played",
    "HOME|CUTOFF": "the program's cutoff",
    "HOME|RESO": "the program's resonance",
    "HOME|DELAY": "the program's delay send",
    "HOME|REVERB": "the reverb level (global)",
    "SAVE|TO": "the slot to save into",
    "SAVE|CURSOR": "the name's character to edit",
    "SAVE|CHAR": "that character",
    "SAVE|MODE": "what SAVE does: p.2",
    "SEQ|LENGTH": "the track's loop",
    "SEQ|QUANT": "to the grid",
    "SEQ|MUTE L": "LOWER silent",
    "SEQ|MUTE U": "UPPER silent",
    "SEQ|BAR": "shown (stopped)",
    "SEQ|PITCH": "scroll the roll",
    "SEQ|TRANSP": "keys transpose",
    "SEQ|ROOT": "transpose's 0",
    "SEQ|CLIP": "1-12 (its name)",
    "SEQ|CURSOR": "in the name",
    "SEQ|CHAR": "that character",
    "SEQ|MODE": "what SAVE does",
}
# the two columns of page 1 (the voice on the left, effects and play on the right; about the same height)
LEFT = ["LFO", "EDIT", "ENV", "ARP"]
RIGHT = ["SEL", "FX", "GLO"]
# long choice lists that read as a range: one line in the knob's cell instead of a list on page 2 (the order is the
# JP's, so a range says it: LFO1 SYNC, Kris 5 Nov)
CHOICE_RANGE = {"LFO|SYNC 1": "OFF, 1/16 .. 8 MEAS"}
# pages with no knobs: a line on what they show instead of four empty cells
PAGE_NOTE = {
    "DEBUG": "CPU (avg, 3 s peak) · voices · octave · MIDI ch · notes in · clock · version",
}
PAGE2 = ["SAVE", "SEQ"]                                 # (page 1 is full since PERFORM: SAVE and SEQ go with their notes)
CTRL_KNOB = {31: "CTRL1", 32: "CTRL2", 37: "CTRL1", 38: "CTRL2"}   # the labels that follow the wave

# The controls that are not pages (the firmware's mvance.c / mv_ui.c; checked against them by hand)
GETTING_AROUND = [
    ("Section button", "Tap: its first page; again: the next (it loops). LFO · EDIT · ENV · SEL · FX · GLO"),
    ("KNOB 1-4", "Edit the four columns, left to right. Turned fast: numbers move x4"),
    ("SELECT", "Single steps on the knob you touched last"),
    ("ALGORITHM", "Walks every page in panel order"),
    ("PRESETS", "Program 1-64 (sends a program change)"),
    ("HOME", "The home screen: program, home knobs, voices, scope"),
    ("ARP", "Tap: the arp on / off (lit when on). Hold 0.5 s: its pages"),
    ("SAVE", "Tap: the SAVE page. SAVE again: do it. Any other button: leave"),
    ("OCT - / +", "Octave -2 .. +2. Hold one, press the other: PART SELECT (- LOWER, + UPPER, both BOTH)"),
    ("PLAY / REC", "PLAY: sequencer on / off (blinks on the beat). REC: arm the part's track; hold 1 s: clear it"),
    ("Key lights", "The notes that sound, in this octave"),
    ("* by the name", "The sound is edited and not saved (SAVE lit)"),
]


def esc(s):
    return html.escape(str(s), quote=True)


def range_text(k):
    """The knob's values as the screen shows them."""
    if k["kind"] == "choice":
        return None
    if k["kind"] == "char":
        return "A-Z a-z 0-9 ..."
    lo, hi = k["min"], k["max"]
    if "centre" in k:
        c = k["centre"]
        return f"{lo - c:+d} .. {hi - c:+d}"
    if k["label"] == "MIDI CH":
        return "OMNI, 1 .. 16"
    return f"{lo} .. {hi}"


def short_choices(ch):
    """Inline when short (a column is narrow), else a pointer to page 2."""
    s = " · ".join(ch)
    return s if len(s) <= 26 else None


def main():
    src, out = sys.argv[1], sys.argv[2]
    ver = sys.argv[3] if len(sys.argv) > 3 else "dev"
    d = json.load(open(src))
    pages = d["pages"]

    # sections in button order, each with its pages in panel order
    order = LEFT + RIGHT + PAGE2
    home = [g for g in pages if g["button"] == "HOME"]     # HOME's knobs go in the PANEL card, not a section card
    pages = [g for g in pages if g["button"] != "HOME"]
    sec = {b: [g for g in pages if g["button"] == b] for b in order}
    missing = [g["button"] for g in pages if g["button"] not in order]
    if missing:
        sys.exit(f"gen_cheatsheet: pages on buttons with no section here: {sorted(set(missing))}")

    lists = []          # (name, choices) for page 2
    pw = {}             # (osc, CTRLn) -> {wave: job}, for page 2
    gaps = []

    def knob_cell(g, k):
        if k is None:
            return '<td class="nk">–</td>'
        label = CTRL_KNOB.get(k.get("byte"), k["label"])
        key = f'{g["title"]}|{label}'
        what = WHAT.get(key)
        if what is None:
            gaps.append(key)
            what = "?"
        val = ""
        if k["kind"] == "choice":
            sc = short_choices(k["choices"]) or CHOICE_RANGE.get(key)
            if sc:
                val = sc
            else:
                ref = f'{g["title"]} {label}'
                if (ref, k["choices"]) not in lists:
                    lists.append((ref, k["choices"]))
                val = f'{len(k["choices"])} choices, p.2'
        else:
            val = range_text(k)
        extra = ""
        if "per_wave" in k:                         # the jobs by wave: a table on page 2
            pw[(g["title"], label)] = k["per_wave"]
            extra = '<div class="pw">job by wave: p.2</div>'
        glob = '<span class="g" title="global: not in the program">G</span>' if k.get("global") else ""
        # label and value on one line when the value is short, else the value under it
        if len(val) <= 11:
            top = f'<div class="kl">{esc(label)}{glob}<span class="kv r">{esc(val)}</span></div>'
            bot = ""
        else:
            top = f'<div class="kl">{esc(label)}{glob}</div>'
            bot = f'<div class="kv">{esc(val)}</div>'
        w = f'<div class="kw">{esc(what)}</div>' if what else ""
        return f"<td>{top}{w}{bot}{extra}</td>"

    cards = {}
    for b in order:
        rows = []
        for g in sec[b]:
            sub = f' <span class="sub">{g["sub"]}/{g["nsub"]}</span>' if g["nsub"] > 1 else ""
            if g["title"] in PAGE_NOTE:             # a page with no knobs (DEBUG): what it shows, across the row
                rows.append(f'<tr><th>{esc(g["title"])}</th><td colspan="4" class="kw">{esc(PAGE_NOTE[g["title"]])}</td></tr>')
                continue
            used = [k for k in g["knobs"] if k is not None]
            if len(used) == 1 and g["knobs"][0] is not None:   # one knob (LFO 3/3 SYNC): one line across the row
                cell = knob_cell(g, used[0])            # (page 1 is full: a row of four cells would not fit)
                cell = cell.replace("<td>", '<td colspan="4" class="one">', 1)
                rows.append(f'<tr><th>{esc(g["title"])}{sub}</th>{cell}</tr>')
                continue
            rows.append(f'<tr><th>{esc(g["title"])}{sub}</th>' + "".join(knob_cell(g, k) for k in g["knobs"]) + "</tr>")
        how = {"ARP": "hold 0.5 s", "SAVE": "tap"}.get(b, "tap, tap again")
        cards[b] = (f'<section class="card"><h2><span class="btn">{b}</span><span class="how">{how}</span></h2>'
                     f'<table><colgroup><col class="c0"><col><col><col><col></colgroup>{"".join(rows)}</table></section>')
    home_html = "".join(knob_cell(home[0], k) for k in home[0]["knobs"]) if home else ""
    if gaps:
        sys.exit("gen_cheatsheet: no WHAT line for: " + ", ".join(gaps))

    ga = "".join(f"<tr><th>{esc(a)}</th><td>{esc(b_)}</td></tr>" for a, b_ in GETTING_AROUND)

    # page 2: the lists, the CCs, MIDI
    lst = "".join(f'<div class="lst"><h3>{esc(n)}</h3><ol start="0">' +
                  "".join(f"<li>{esc(c)}</li>" for c in ch) + "</ol></div>" for n, ch in lists)
    pw_html = ""
    for osc in sorted({o for o, _ in pw}):
        c1, c2 = pw.get((osc, "CTRL1"), {}), pw.get((osc, "CTRL2"), {})
        rows = "".join(f"<tr><td>{esc(w)}</td><td>{esc(c1.get(w, ''))}</td><td>{esc(c2.get(w, ''))}</td></tr>" for w in c1)
        pw_html += (f'<div class="lst pwt"><h3>{esc(osc)} CTRL by wave</h3><table><tr><th>WAVE</th><th>CTRL1</th>'
                    f'<th>CTRL2</th></tr>{rows}</table></div>')
    ccs = sorted({(k["cc"], f'{g["title"]} {CTRL_KNOB.get(k.get("byte"), k["label"])}')
                  for g in pages for k in g["knobs"] if k and "cc" in k})
    cc_rows = "".join(f"<tr><td>{c}</td><td>{esc(n)}</td></tr>" for c, n in ccs)
    half = (len(ccs) + 1) // 2
    cc_a = "".join(f"<tr><td>{c}</td><td>{esc(n)}</td></tr>" for c, n in ccs[:half])
    cc_b = "".join(f"<tr><td>{c}</td><td>{esc(n)}</td></tr>" for c, n in ccs[half:])
    del cc_rows

    page = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>M-vance cheat sheet</title>
<!-- Generated by tools/gen_cheatsheet.py from the firmware's panel (build/pages.json). Do not edit by hand. -->
<style>
:root {{ --ink:#1b1712; --mute:#6b6257; --rule:#d9d1c4; --amber:#c86a00; --paper:#fffdf9; --card:#fbf6ee; }}
@page {{ size: A4 landscape; margin: 6mm; }}
* {{ box-sizing: border-box; }}
html {{ background: #e9e4dc; }}
body {{ margin: 0; color: var(--ink); background: #e9e4dc;
  font: 6.5pt/1.13 "Helvetica Neue", Arial, sans-serif; -webkit-print-color-adjust: exact; print-color-adjust: exact; }}
.sheet {{ width: 283mm; min-height: 196mm; margin: 6mm auto; padding: 5mm 6mm; background: var(--paper);
  box-shadow: 0 1px 6px rgba(0,0,0,.15); break-after: page; }}
.sheet:last-child {{ break-after: auto; }}
header {{ display: flex; align-items: baseline; gap: 4mm; border-bottom: 1.4pt solid var(--amber); padding-bottom: 1.5mm; margin-bottom: 2.5mm; }}
header h1 {{ margin: 0; font-size: 13pt; letter-spacing: .06em; }}
header h1 b {{ color: var(--amber); }}
header .tag {{ color: var(--mute); }}
header .ver {{ margin-left: auto; color: var(--mute); font-variant-numeric: tabular-nums; }}
.grid {{ display: grid; grid-template-columns: 1fr 1fr; gap: 0 3mm; align-items: start; }}
.card {{ background: var(--card); border: .6pt solid var(--rule); border-radius: 1.4mm; padding: 1.2mm 1.8mm 1.3mm;
  break-inside: avoid; margin-bottom: 2.2mm; display: inline-block; width: 100%; }}
.card h2 {{ margin: 0 0 1mm; font-size: 8.6pt; display: flex; align-items: center; gap: 2mm; }}
.btn {{ display: inline-block; background: var(--ink); color: #fff; border-radius: 1mm; padding: .2mm 1.6mm; letter-spacing: .05em; }}
.how {{ color: var(--mute); font-weight: normal; font-size: 7pt; }}
table {{ width: 100%; border-collapse: collapse; table-layout: fixed; }}
col.c0 {{ width: 15%; }}
th, td {{ text-align: left; vertical-align: top; padding: .5mm 1mm; border-top: .5pt solid var(--rule); }}
tr:first-child th, tr:first-child td {{ border-top: 0; }}
th {{ font-size: 7.4pt; }}
.sub {{ color: var(--mute); font-weight: normal; }}
.kl {{ font-weight: bold; letter-spacing: .03em; }}
.kw {{ color: var(--ink); }}
.kv {{ color: var(--amber); font-variant-numeric: tabular-nums; font-weight: normal; }}
.kv.r {{ float: right; letter-spacing: 0; }}
.pw {{ color: var(--mute); font-size: 6.4pt; margin-top: .4mm; }}
.nk {{ color: var(--rule); }}
.g {{ display: inline-block; margin-left: 1mm; font-size: 5.8pt; color: #fff; background: var(--amber); border-radius: .8mm; padding: 0 .8mm; vertical-align: 1px; }}
.around {{ margin-bottom: 2.4mm; }}
.homek {{ margin-bottom: 1mm; }} .homek th {{ width: 15%; }}
.around2 {{ display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 0 4mm; }}
.around2 th {{ width: 30%; }}
.lists {{ display: grid; grid-template-columns: repeat(7, 1fr); gap: 2mm 3mm; }}
.seqcard {{ grid-column: span 3; zoom: .78; }}  /* the SEQ card in the lists' free corner, a little smaller (page 2 is full below) */
td.one > div {{ display: inline; margin-right: 2.5mm; }}   /* a one-knob page: label, value, what on one line */
td.one .kv.r {{ float: none; margin-left: 1.5mm; }}
.pwt {{ grid-column: span 1; }} .pwt th, .pwt td {{ padding: .2mm .6mm; border: 0; }}
.lst h3 {{ margin: 0 0 .6mm; font-size: 7.4pt; color: var(--amber); }}
.lst ol {{ margin: 0; padding-left: 5mm; }}
.lst li::marker {{ color: var(--mute); font-variant-numeric: tabular-nums; }}
h2.p2 {{ font-size: 8.4pt; margin: 2mm 0 1mm; border-bottom: .6pt solid var(--rule); padding-bottom: .6mm; }}
.cols3 {{ display: grid; grid-template-columns: 1fr 1fr 1.3fr; gap: 0 4mm; }}
.cc td:first-child {{ width: 9mm; color: var(--amber); font-variant-numeric: tabular-nums; }}
ul.notes {{ margin: 0; padding-left: 4mm; }}
ul.notes li {{ margin-bottom: .35mm; }}
footer {{ margin-top: 2.5mm; color: var(--mute); font-size: 6.4pt; }}
@media screen and (max-width: 300mm) {{ .sheet {{ width: auto; margin: 0; box-shadow: none; }}
  .grid, .around2, .cols3 {{ grid-template-columns: 1fr; }} .lists {{ grid-template-columns: repeat(2, 1fr); }} }}
@media print {{ html, body {{ background: #fff; }} .sheet {{ margin: 0; box-shadow: none; width: auto; min-height: 0; padding: 0; }} }}
</style>
</head>
<body>
<div class="sheet">
<header><h1><b>M-VANCE</b> CHEAT SHEET</h1><span class="tag">JP-8000/8080-style virtual analog for the M-VAVE FM-1 · the JT-8000 engine in fixed point</span><span class="ver">firmware {esc(ver)}</span></header>
<section class="card around"><h2><span class="btn">PANEL</span><span class="how">getting around</span></h2>
<table class="homek"><tr><th>HOME knobs</th>{home_html}</tr></table>
<div class="around2">{"".join("<table>" + "".join(f"<tr><th>{esc(a)}</th><td>{esc(b_)}</td></tr>" for a, b_ in GETTING_AROUND[i:i + 4]) + "</table>" for i in (0, 4, 8))}</div></section>
<div class="grid"><div>{"".join(cards[b] for b in LEFT)}</div><div>{"".join(cards[b] for b in RIGHT)}</div></div>
<footer><span class="g">G</span> global setting: belongs to the FM-1, not to the program. Amber: the values the screen shows. Generated from the firmware's own page table.</footer>
</div>

<div class="sheet">
<header><h1><b>M-VANCE</b> LISTS · MIDI</h1><span class="ver">firmware {esc(ver)} · {d["programs"]} programs · {d["voices"]} voices</span></header>
<h2 class="p2">Choices (the number is the stored value; 0 first)</h2>
<div class="lists">{lst}{pw_html}<div class="seqcard">{cards["SEQ"]}</div></div>
<div class="cols3">
<div><h2 class="p2">CC map (JP-8080 MODE2), in and out</h2><table class="cc">{cc_a}</table></div>
<div><h2 class="p2">&nbsp;</h2><table class="cc">{cc_b}</table></div>
<div>
<h2 class="p2">MIDI</h2>
<ul class="notes">
<li><b>Notes</b> in from USB and the TRS jack; the keys send on the GLOBAL MIDI CH (OMNI: out on 1).</li>
<li><b>Program change</b> in selects 1-64; PRESETS sends one.</li>
<li><b>Pitch bend</b> by the program's BEND + / BEND -. <b>CC 1</b> mod wheel brings LFO 2 in.</li>
<li><b>Every knob with a CC</b> sends it when turned, so a DAW records the panel; incoming CCs edit the sound.</li>
<li><b>Clock</b> (24 a beat) sets the arp's and the sequencer's tempo while it runs; <b>START</b> restarts the arp on step 1 and starts the sequencer from bar 1, locked to the clock (CONTINUE: the same); <b>STOP</b> stops it.</li>
</ul>
<h2 class="p2">SAVE page</h2>
{cards["SAVE"]}
<ul class="notes">
<li><b>WRITE</b>: the sound as edited into slot TO (a copy, if TO is another slot). The name: CURSOR and CHAR.</li>
<li><b>SWAP</b>: the program playing and slot TO trade places (reorder the bank).</li>
<li><b>FACTORY</b>: slot TO back to the factory sound.</li>
<li>A <b>*</b> on the home screen: the sound is edited and not saved. Saving to flash happens once nothing sounds.</li>
</ul>
<h2 class="p2">Kept where</h2>
<ul class="notes">
<li><b>In the program</b> (64 slots): everything on LFO, EDIT, ENV, SEL, TONE, MULTI-FX, DELAY, PLAY MODE, and PERFORM (except PANEL): a program is a performance, two patches (UPPER, LOWER) and how they play.</li>
<li><b>Layered</b> (DUAL / SPLIT): the voice pages edit the PANEL part (header U / L / L+U); TONE, MULTI-FX and DELAY are one chain, UPPER's.</li>
<li><b>On the FM-1</b> (<span class="g">G</span>): REVERB, ARP, SEQ and its 12 clips (the notes playing: RAM only), MIDI CH, TEMPO, the octave, the program last played. VOICES starts at 8 at power-on.</li>
</ul>
</div>
</div>
<footer>M-vance is open source (GPL-3.0). JP-8000 and JP-8080 are trademarks of Roland Corporation; M-VAVE and FM-1 of their owners. M-vance is not made by or affiliated with them.</footer>
</div>
</body>
</html>
"""
    open(out, "w").write(page)
    print(f"gen_cheatsheet: {out} ({len(pages)} pages, {len(lists)} lists, {len(ccs)} CCs)")


if __name__ == "__main__":
    main()
