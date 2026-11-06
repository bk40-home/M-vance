// SPDX-License-Identifier: GPL-3.0-only
// Round-trip test for sim/bank/bank-model.js, run on the host with Node (no browser, no emcc):
//   python3 tools/make_bank.py tools/bank/public.txt     # -> build/mvance-public.syx (if not already built)
//   node sim/bank/bank-test.mjs                           # from the repo root
// Proves the model parses and re-serialises a make_bank.py bank byte-for-byte, that it is idempotent, and that
// the reorder / rename operations survive a round trip.
import { readFileSync } from 'node:fs';
import { parseBankFile, makeBankSyx, moveSlot, renameSlot } from './bank-model.js';

const orig = new Uint8Array(readFileSync('build/mvance-public.syx'));
let fail = 0;
const ok = (cond, msg) => { console.log((cond ? 'OK   ' : 'FAIL ') + msg); if (!cond) fail = 1; };

const perfs = () => parseBankFile(orig).filter((i) => i.kind === 'P');
const P = perfs();
ok(P.length === 64, `parsed ${P.length} performances (want 64)`);

// 1. byte-identity with make_bank.py's own .syx
const s1 = makeBankSyx(P);
let same = s1.length === orig.length, at = -1;
if (same) for (let i = 0; i < orig.length; i++) if (s1[i] !== orig[i]) { same = false; at = i; break; }
ok(same, same ? `serialised bank byte-identical to make_bank output (${s1.length} B)`
              : `serialised bank differs (len ${s1.length} vs ${orig.length}, first diff @${at})`);

// 2. idempotent: parse(serialise) -> serialise again == the same bytes
const s2 = makeBankSyx(parseBankFile(s1).filter((i) => i.kind === 'P'));
ok(s2.length === s1.length && s2.every((b, i) => b === s1[i]), 're-parse / re-serialise is idempotent');

// 3. reorder: move slot 1 to position 2, serialise, re-parse, names swapped
const b = perfs(); const n0 = b[0].name, n1 = b[1].name;
moveSlot(b, 0, 1);
const re = parseBankFile(makeBankSyx(b)).filter((i) => i.kind === 'P');
ok(re[0].name === n1 && re[1].name === n0, `reorder: slots 1&2 are now '${re[0].name}' , '${re[1].name}'`);

// 4. rename survives a round trip
renameSlot(b[0], 'MY LEAD');
const re2 = parseBankFile(makeBankSyx(b)).filter((i) => i.kind === 'P');
ok(re2[0].name === 'MY LEAD', `rename slot 1 -> '${re2[0].name}'`);

process.exit(fail);
