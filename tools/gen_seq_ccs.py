#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The sound's CCs for the sequencer's automation (Kris 20 Nov 2026: fm1-port/mvance-seq-automation-proposal-
2026-11-20.md): every JP MODE2 CC that moves a sound parameter (firmware/gen/mv_params.h MV_CC_PARAM, the table
mv_cc_apply uses), with a name from its section and panel label. One copy for the tools and the page:

    python3 tools/gen_seq_ccs.py [-o sim/seq-ccs.js]   (sim/build.sh runs it; mid2clip.py imports sound_ccs())

Standard library only."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SEC = {'LFO1': 'LFO1', 'LFO2': 'LFO2', 'OSCC': 'OSC', 'PITCH': 'PITCH', 'PENV': 'P.ENV', 'OSC1': 'OSC1',
       'OSC2': 'OSC2', 'FILT': 'FILTER', 'FENV': 'F.ENV', 'AMP': 'AMP', 'AENV': 'A.ENV', 'FX': 'FX',
       'VOICE': 'VOICE', 'CTRL': 'CTRL', 'VELO': 'VELO', 'RSVD': ''}


def sound_ccs(header=ROOT / 'firmware/gen/mv_params.h'):
    """{cc: name} of the CCs mv_cc_apply turns into a sound parameter"""
    src = Path(header).read_text()
    table = re.search(r'MV_CC_PARAM\[128\] = \{(.*?)\};', src, re.S).group(1)
    ids = [int(x) for x in re.sub(r'/\*.*?\*/', ' ', table, flags=re.S).split(',') if x.strip()]
    params = re.search(r'MV_PARAMS\[MV_NPARAMS\] = \{(.*?)\n\};', src, re.S).group(1)
    rows = re.findall(r'\{\d+, \d+, \d+, \d+, \d+, \d+, MV_D_\w+, MV_SEC_(\w+), MV_ST_\w+, \d+, "([^"]*)"', params)
    out = {}
    for cc, pid in enumerate(ids):
        if pid != 255 and pid < len(rows):
            sec, label = rows[pid]
            out[cc] = (SEC.get(sec, sec) + ' ' + label).strip()
    return out


def main(argv):
    out = argv[argv.index('-o') + 1] if '-o' in argv else str(ROOT / 'sim/seq-ccs.js')
    ccs = sound_ccs()
    body = ',\n'.join(f'  {cc}: {name!r}'.replace("'", '"') for cc, name in sorted(ccs.items()))
    Path(out).write_text('// SPDX-License-Identifier: GPL-3.0-only\n'
                         '// Made by tools/gen_seq_ccs.py from firmware/gen/mv_params.h: do not edit. The CCs the\n'
                         '// sequencer\'s automation records (the sound parameters mv_cc_apply moves), with their names.\n'
                         f'export const SOUND_CCS = {{\n{body},\n}};\n', newline='\n')
    print(f'{out}: {len(ccs)} CCs')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
