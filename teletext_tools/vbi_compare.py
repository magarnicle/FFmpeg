#!/usr/bin/env python3
"""
Structural analyser / comparator for AJA ntv2line21grab VBI dumps (teletext).

Parses one or more `*.txt` captures (the "=== FRAME DUMP ===" format with
per-line `min/max/mean/spread` headers and `luma:` hex blocks) and reports the
structure of the WST teletext on lines 21 (field 1) and 334 (field 2):
  - which VBI lines carry signal (spread > SIG_THRESH)
  - per-line: AJA offset, level min/max, CRI start sample, distinct luma levels
  - packet kinds (headers vs display rows vs X/mm/rr data packets)
  - header page numbers, subcodes, control bits (C4/C7/C8/C9/C11/region), byte 7
  - box structure (double-height, Start Box 0x0b, End Box 0x0a or parity 0x8a)
  - whether the two fields carry different packets (dual-field)

Usage:
    python3 vbi_compare.py capture.txt                 # analyse one
    python3 vbi_compare.py deck.txt poli.txt           # analyse several
    python3 vbi_compare.py --json out.json a.txt b.txt  # also dump raw JSON

Notes:
  - Teletext bit rate 6.9375 Mbit/s at 13.5 MHz sampling => 1.9459 samples/bit.
  - CRI start is found by sweeping the offset for the one that yields framing 0x27.
  - Control bytes are Hamming 8/4; nearest-codeword decode tolerates capture noise.
  - A parity-encoded End Box is 0x8a (0x0a | odd-parity); counted as End Box.
"""
import re
import sys
import json
from collections import Counter

SPB = 13.5 / 6.9375                # samples per teletext bit
SIG_THRESH = 40                    # spread above this = the line carries signal
TT_LINES = ("L21F1", "L334F2")     # the two SD-PAL teletext lines

HAM = [0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F,
       0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA]
# nearest-codeword Hamming 8/4 decode for every possible byte (noise-tolerant)
DEH = [min(range(16), key=lambda v: bin(HAM[v] ^ b).count("1")) for b in range(256)]

LINE_RE = re.compile(
    r"^(L\s*\d+ F\d)\s+\[off=\s*(\d+)\]\s+min=\s*(\d+)\s+max=\s*(\d+)"
    r"\s+mean=\s*(\d+)\s+spread=\s*(\d+)")


def parse(path):
    """Return (frames, siglines, all_lines).

    frames: list of {label: {off,mn,mx,mean,spread,vals}}; vals only for TT_LINES.
    siglines: Counter of label -> frames where spread > SIG_THRESH.
    all_lines: sorted set of every line label seen.
    """
    L = open(path).read().splitlines()
    frames, cur, i = [], None, 0
    sig, all_lines = Counter(), set()
    while i < len(L):
        if L[i].startswith("Frame:"):
            if cur is not None:
                frames.append(cur)
            cur = {}
        m = LINE_RE.match(L[i])
        if m and cur is not None:
            lab = m.group(1).replace(" ", "")
            all_lines.add(lab)
            spread = int(m.group(6))
            if spread > SIG_THRESH:
                sig[lab] += 1
            vals = None
            if lab in TT_LINES and i + 1 < len(L) and "luma:" in L[i + 1]:
                j = i + 1
                lu = L[j].split("luma:")[1].strip()
                j += 1
                while (j < len(L) and L[j].startswith(" ")
                       and "raw:" not in L[j] and "luma:" not in L[j]):
                    lu += L[j].strip()
                    j += 1
                vals = bytes(int(lu[k:k + 2], 16) for k in range(0, len(lu) - 1, 2))
                i = j
            if lab in TT_LINES:
                cur[lab] = dict(off=int(m.group(2)), mn=int(m.group(3)),
                                mx=int(m.group(4)), mean=int(m.group(5)),
                                spread=spread, vals=vals)
                continue
        i += 1
    if cur is not None:
        frames.append(cur)
    return frames, dict(sig), sorted(all_lines)


HAMSET = set(HAM)


def slice_line(v, start, spb, phase):
    """Slice one luma line at a given bit timing; return (framing, [bytes]) or None.

    A single fixed timing is NOT enough: a heavily band-limited waveform (e.g. the
    Deltacast/Polistream eye) samples best at a different start/phase/spb than our
    sharper DeckLink eye. Use calibrate() to recover the timing per file first.
    """
    hi, lo = max(v), min(v)
    mid = (hi + lo) // 2
    bits = []
    for b in range(370):
        idx = int(round(start + phase + spb * b))
        if idx >= len(v):
            break
        bits.append(1 if v[idx] > mid else 0)
    if len(bits) < 200:
        return None
    fv = sum(bits[16 + k] << k for k in range(8))
    by = [sum(bits[24 + 8 * n + k] << k for k in range(8))
          for n in range((len(bits) - 24) // 8)]
    return fv, by


def calibrate(lines):
    """Find (spb, start, phase) that maximises valid-Hamming header bytes.

    Returns (valid_fraction, spb, start, phase). Uses the first lines that frame-lock
    on 0x27. Nominal spb is 13.5/6.9375=1.9459 but the best sampling trajectory through
    a shaped waveform is often a little lower; sweep a small window around it.
    """
    best = None
    for spb in [x / 1000 for x in range(1928, 1962, 1)]:
        for start in range(0, 10):
            for phase in [p / 10 for p in range(-10, 11)]:
                good = tot = 0
                for v in lines[:20]:
                    r = slice_line(v, start, spb, phase)
                    if not r or r[0] != 0x27:
                        continue
                    for pos in range(10):
                        tot += 1
                        good += (r[1][pos] in HAMSET)
                if tot > 50:
                    s = good / tot
                    if best is None or s > best[0]:
                        best = (s, spb, start, phase)
    return best or (0.0, SPB, 0, 0.0)


def decode_at(v, spb, start, phase):
    """Decode one line at a recovered timing; return {st, mag, row, by} or None."""
    r = slice_line(v, start, spb, phase)
    if not r or r[0] != 0x27:
        return None
    by = r[1]
    m0 = DEH[by[0]]
    return dict(st=start, mag=m0 & 7, row=((m0 >> 3) & 1) | (DEH[by[1]] << 1), by=by)


def summarise(path):
    frames, sig, all_lines = parse(path)
    R = dict(nframes=len(frames), siglines=sig,
             siglines_count=len(sig), all_lines_max=all_lines[-1] if all_lines else None)
    # Recover the bit timing once per file (per teletext line) before decoding.
    cal = {}
    for lab in TT_LINES:
        vv = [fr[lab]["vals"] for fr in frames
              if lab in fr and fr[lab]["spread"] > SIG_THRESH and fr[lab]["vals"]]
        cal[lab] = calibrate(vv) if vv else (0.0, SPB, 0, 0.0)
    R["timing"] = {lab: dict(valid=round(cal[lab][0], 3), spb=cal[lab][1],
                             start=cal[lab][2], phase=cal[lab][3]) for lab in TT_LINES}
    for lab in TT_LINES:
        _sc, spb, start, phase = cal[lab]
        vv = [fr[lab] for fr in frames
              if lab in fr and fr[lab]["spread"] > SIG_THRESH and fr[lab]["vals"]]
        kinds, cri, byte7, subc, pages = (Counter() for _ in range(5))
        C = {k: Counter() for k in ("C4", "C7", "C8", "C9", "C11", "region")}
        box = {k: Counter() for k in ("dh", "sb", "eb")}
        for d in vv:
            dd = decode_at(d["vals"], spb, start, phase)
            if dd is None:
                kinds["NOFRAME"] += 1
                continue
            cri[dd["st"]] += 1
            by = dd["by"]
            if dd["row"] == 0:
                pg = "8%x%x" % (DEH[by[3]], DEH[by[2]])
                pages[pg] += 1
                kinds[("HDR", dd["mag"], pg)] += 1
                byte7[by[7]] += 1
                subc[(DEH[by[4]], DEH[by[5]] & 7, DEH[by[6]], DEH[by[7]] & 3)] += 1
                C["C4"][(DEH[by[5]] >> 3) & 1] += 1
                b8 = DEH[by[8]]
                C["C7"][b8 & 1] += 1
                C["C8"][(b8 >> 1) & 1] += 1
                C["C9"][(b8 >> 2) & 1] += 1
                b9 = DEH[by[9]]
                C["C11"][b9 & 1] += 1
                C["region"][(b9 >> 1) & 7] += 1
            else:
                kinds[("ROW", dd["mag"], dd["row"])] += 1
                disp = by[2:42]
                box["dh"][0x0d in disp[:6]] += 1
                box["sb"][disp.count(0x0b)] += 1
                box["eb"][disp.count(0x0a) + disp.count(0x8a)] += 1
        R[lab] = dict(
            n=len(vv),
            off=dict(Counter(d["off"] for d in vv)),
            min=dict(Counter(d["mn"] for d in vv)),
            max=dict(Counter(d["mx"] for d in vv)),
            mean=dict(Counter(d["mean"] for d in vv)),
            distinct_levels=dict(Counter(len(set(d["vals"])) for d in vv)),
            cri_start=dict(cri),
            kinds={str(k): v for k, v in kinds.items()},
            pages=dict(pages),
            byte7={hex(k): v for k, v in byte7.items()},
            subcode={str(k): v for k, v in subc.items()},
            ctrl={k: dict(c) for k, c in C.items()},
            box={k: dict(c) for k, c in box.items()})
    diff = sum(1 for fr in frames if "L21F1" in fr and "L334F2" in fr
               and fr["L21F1"]["vals"] != fr["L334F2"]["vals"])
    R["fields_differ"] = "%d/%d" % (diff, len(frames))
    return R


def main():
    args = sys.argv[1:]
    json_out = None
    if args and args[0] == "--json":
        json_out = args[1]
        args = args[2:]
    if not args:
        print(__doc__)
        sys.exit(1)
    res = {}
    for path in args:
        res[path] = summarise(path)
        r = res[path]
        print("\n############ %s ############" % path)
        print(" nframes=%d  signal-bearing VBI lines=%d  fields_differ=%s"
              % (r["nframes"], r["siglines_count"], r["fields_differ"]))
        print(" recovered timing: %s" % r["timing"])
        for lab in TT_LINES:
            l = r[lab]
            print(" [%s] n=%d off=%s cri_start=%s min=%s max=%s mean=%s distinct=%s"
                  % (lab, l["n"], l["off"], l["cri_start"], l["min"], l["max"],
                     l["mean"], l["distinct_levels"]))
            print("      pages=%s byte7=%s subcode=%s"
                  % (l["pages"], l["byte7"], l["subcode"]))
            print("      ctrl=%s" % l["ctrl"])
            print("      box=%s kinds=%s" % (l["box"], l["kinds"]))
    if json_out:
        open(json_out, "w").write(json.dumps(res, indent=1, default=str))
        print("\nwrote %s" % json_out)


if __name__ == "__main__":
    main()
