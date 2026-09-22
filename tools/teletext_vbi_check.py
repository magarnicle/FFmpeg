#!/usr/bin/env python3
"""Check an SD PAL teletext VBI capture against the WST/OP-42 invariants.

Input is an AJA frame dump (the ``ntv2`` --dump text format): one
``=== FRAME DUMP ===`` block per frame, a ``L<line> F<field> [off=..] min=..``
header per VBI line, and ``luma:``/``raw:`` hex for the lines that carry data.

The script slices every teletext line in the capture, decodes the packets, and
reports the measurements that a teletext inserter can get wrong without any
downstream device complaining: bit rate, h-timing against the OP-42 12 us
datum, edge shape, signal levels, packet structure and per-field continuity.

With ``--expect-*`` options it exits non-zero when a measurement is out of
tolerance, which is the point: run it after any change to the waveform
generator or the packet scheduler.

Example:
    tools/teletext_vbi_check.py deck.txt --expect-offset 6 --monotonic
    tools/teletext_vbi_check.py deck.txt --reference poli.txt
"""

import argparse
import cmath
import collections
import math
import re
import statistics
import sys

# 625/50 at 13.5 MHz. The teletext bit period is exactly 13.5/6.9375 = 72/37
# samples; anything else is a bug in the generator, not a tolerance.
SAMPLE_RATE_HZ = 13.5e6
BIT_PERIOD = 72.0 / 37.0
NS_PER_SAMPLE = 1e9 / SAMPLE_RATE_HZ
PACKET_BYTES = 45                  # 2 clock run-in + 1 framing + 42 data
RUN_IN = [1, 0] * 8
FRAMING = [1, 1, 1, 0, 0, 1, 0, 0]  # 0x27, LSB first
# ITU-R BT.656: 0H to the start of the 625-line digital active line.
BT656_0H_OFFSET = 132
# OP-42 Fig 2/3: the penultimate of the eight run-in "1"s, i.e. run-in bit 12.
OP42_DATUM_US = 12.0
OP42_DATUM_TOL_US = 0.288
PENULTIMATE_RUN_IN_BIT = 12
# Nominal levels, 8-bit: blanking/black and the 66% "1" level.
LEVEL_LOW = 16
LEVEL_HIGH = 160

HAM_ENCODE = [0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F,
              0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA]
HAM_DECODE = {v: i for i, v in enumerate(HAM_ENCODE)}


def parse_dump(path):
    """Yield one dict per frame: {'Frame': .., 'lines': {(line, field): {...}}}."""
    frame = None
    cur = None
    mode = None
    with open(path) as handle:
        for text in handle:
            if text.startswith('=== FRAME DUMP'):
                if frame:
                    yield frame
                frame, cur, mode = {'lines': {}}, None, None
                continue
            if frame is None:
                continue
            if not text[:1].isspace():
                head = re.match(r'^(\w+): (.*)$', text.strip())
                if head:
                    frame[head.group(1)] = head.group(2)
                stat = re.match(r'^L\s*(\d+) F(\d) \[off=\s*\d+\]\s+min=\s*(\d+) '
                                r'max=\s*(\d+) mean=\s*(\d+) spread=\s*(\d+)', text)
                if stat:
                    key = (int(stat.group(1)), int(stat.group(2)))
                    cur = frame['lines'].setdefault(key, {'luma': '', 'raw': ''})
                    mode = None
                continue
            named = re.match(r'^\s+(luma|raw):\s+([0-9a-f]+)\s*$', text)
            if named:
                mode = named.group(1)
                cur[mode] += named.group(2)
                continue
            more = re.match(r'^\s+([0-9a-f]+)\s*$', text)
            if more and mode and cur is not None:
                cur[mode] += more.group(1)
    if frame:
        yield frame


def unhex(text):
    return [int(text[i:i + 2], 16) for i in range(0, len(text), 2)]


def interp(samples, x):
    i = int(math.floor(x))
    if i < 0:
        return float(samples[0])
    if i + 1 >= len(samples):
        return float(samples[-1])
    return samples[i] + (samples[i + 1] - samples[i]) * (x - i)


def zero_crossings(samples, level):
    out = []
    prev = samples[0] - level
    for i in range(1, len(samples)):
        cur = samples[i] - level
        if (prev < 0) != (cur < 0) and cur != 0:
            out.append(i - 1 + (level - samples[i - 1]) / (samples[i] - samples[i - 1]))
        prev = cur
    return out


def slice_packet(samples):
    """Recover clock and data from one VBI line. Returns None if it is not WST."""
    low, high = min(samples), max(samples)
    if high - low < 40:
        return None
    thr = (low + high) / 2.0
    crossings = zero_crossings(samples, thr)
    if len(crossings) < 16:
        return None
    phase = cmath.phase(sum(cmath.exp(2j * math.pi * x / BIT_PERIOD)
                            for x in crossings)) / (2 * math.pi) * BIT_PERIOD
    nbits = PACKET_BYTES * 8
    best = None
    for k in range(-30, 45):
        for half in (0.0, 0.5):
            start = phase + (k + half) * BIT_PERIOD
            if start < -1 or start + (nbits - 1) * BIT_PERIOD > len(samples) + 1:
                continue
            head = [1 if interp(samples, start + i * BIT_PERIOD) > thr else 0
                    for i in range(24)]
            if head != RUN_IN + FRAMING:
                continue
            values = [interp(samples, start + i * BIT_PERIOD) for i in range(nbits)]
            margin = min(abs(v - thr) for v in values)
            if best is None or margin > best[0]:
                bits = [1 if v > thr else 0 for v in values]
                data = [sum(bits[i * 8 + j] << j for j in range(8))
                        for i in range(PACKET_BYTES)]
                best = (margin, start, bits, data)
    if best is None:
        return None
    return {'margin': best[0], 'start': best[1], 'bits': best[2], 'bytes': best[3],
            'thr': thr, 'low': low, 'high': high}


def fit_bit_period(samples, sliced):
    """Least-squares bit period from the position of every data transition."""
    crossings = zero_crossings(samples, sliced['thr'])
    bits = sliced['bits']
    points = []
    for i in range(1, len(bits)):
        if bits[i] == bits[i - 1]:
            continue
        predicted = sliced['start'] + (i - 0.5) * BIT_PERIOD
        nearest = min(crossings, key=lambda x: abs(x - predicted))
        if abs(nearest - predicted) < 0.7:
            points.append((i - 0.5, nearest))
    if len(points) < 50:
        return None
    n = len(points)
    sx = sum(x for x, _ in points)
    sy = sum(y for _, y in points)
    sxx = sum(x * x for x, _ in points)
    sxy = sum(x * y for x, y in points)
    slope = (n * sxy - sx * sy) / (n * sxx - sx * sx)
    return slope, (sy - slope * sx) / n


def sine_squared_step(x):
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    return 0.5 - 0.5 * math.cos(math.pi * x)


def fit_edge(profile_x, profile_y, rising):
    """Fit a sine-squared edge to a measured transition, in ns (10-90%).

    The profile comes from interpolating a 13.5 MHz capture between samples,
    which biases a smooth edge wide by several tens of ns -- linear
    interpolation always cuts the corner, and averaging many transitions does
    not cancel it because the error has a sign. So the candidate edge is put
    through the same path before it is compared: sampled on an integer grid at
    eight sub-sample phases, interpolated back to the profile offsets, and
    averaged, exactly as edge_profile() does to the real thing.
    """
    lo, hi = (LEVEL_LOW, LEVEL_HIGH) if rising else (LEVEL_HIGH, LEVEL_LOW)
    phases = [p / 8.0 for p in range(8)]

    def level(u, full):
        return lo + (hi - lo) * sine_squared_step(u / full + 0.5)

    def model_profile(full, centre):
        out = [0.0] * len(profile_x)
        for phase in phases:
            for n, x in enumerate(profile_x):
                grid = x - centre - phase
                i = math.floor(grid)
                frac = grid - i
                a = level(i + phase, full)
                b = level(i + 1 + phase, full)
                out[n] += a + (b - a) * frac
        return [v / len(phases) for v in out]

    def error(full, centre):
        return sum((m - y) ** 2 for m, y in zip(model_profile(full, centre), profile_y))

    full, centre, step = 3.0, 0.0, 0.5
    for _ in range(3):
        best = None
        for cand_full in [full + i * step for i in range(-8, 9) if full + i * step > 0.05]:
            for cand_centre in [centre + i * step / 2 for i in range(-8, 9)]:
                err = error(cand_full, cand_centre)
                if best is None or err < best[0]:
                    best = (err, cand_full, cand_centre)
        _, full, centre = best
        step /= 8.0
    return full * 0.590334 * NS_PER_SAMPLE        # 10-90% is 0.590334 of the width


def edge_profile(lines, rising):
    """Average an isolated transition across every line, at 1/16-sample steps."""
    acc = collections.defaultdict(list)
    want_before = 0 if rising else 1
    for samples, sliced in lines:
        bits = sliced['bits']
        for k in range(3, len(bits) - 3):
            if bits[k - 3:k] != [want_before] * 3:
                continue
            if bits[k:k + 3] != [1 - want_before] * 3:
                continue
            centre = sliced['start'] + k * BIT_PERIOD - BIT_PERIOD / 2
            for step in range(-32, 41):
                acc[step].append(interp(samples, centre + step * 0.0625))
    if not acc:
        return None, None
    xs = sorted(acc)
    return [x * 0.0625 for x in xs], [statistics.mean(acc[x]) for x in xs]


def ten_ninety(profile_x, profile_y, rising):
    def cross(level):
        for i in range(1, len(profile_y)):
            a, b = profile_y[i - 1], profile_y[i]
            if (a - level) * (b - level) <= 0 and a != b:
                return profile_x[i - 1] + (level - a) / (b - a) * (profile_x[i] - profile_x[i - 1])
        return None
    lo = LEVEL_LOW + 0.1 * (LEVEL_HIGH - LEVEL_LOW)
    hi = LEVEL_LOW + 0.9 * (LEVEL_HIGH - LEVEL_LOW)
    first, second = (cross(lo), cross(hi)) if rising else (cross(hi), cross(lo))
    if first is None or second is None:
        return None
    return (second - first) * NS_PER_SAMPLE


def row_address(packet):
    a0 = HAM_DECODE.get(packet[3])
    a1 = HAM_DECODE.get(packet[4])
    if a0 is None or a1 is None:
        return None
    return (a0 | (a1 << 4)) >> 3


def odd_parity(byte):
    return bin(byte).count('1') & 1


class Report:
    def __init__(self):
        self.failures = []

    def line(self, text):
        print(text)

    def check(self, ok, text):
        print('  %s %s' % ('PASS' if ok else 'FAIL', text))
        if not ok:
            self.failures.append(text)


def render_line(packet, offset, rise_ns, width=720):
    """Render one teletext line the way the DeckLink generator does.

    Mirrors generate_teletext_vbi_waveform()'s sine-squared edge path: the bit
    sequence is laid out on the exact 72/37 grid and every bit boundary
    contributes a monotonic sine-squared step. Used by --self-test to check the
    measurement code against a waveform whose parameters are known exactly.
    """
    bits = list(RUN_IN) + list(FRAMING)
    for byte in packet:
        bits += [(byte >> i) & 1 for i in range(8)]
    levels = [LEVEL_HIGH if b else LEVEL_LOW for b in bits]
    full = max((rise_ns / NS_PER_SAMPLE) / 0.590334, 1e-3)
    acc = [0.0] * width
    for k in range(len(levels) + 1):
        before = LEVEL_LOW if k == 0 else levels[k - 1]
        after = LEVEL_LOW if k == len(levels) else levels[k]
        delta = after - before
        if not delta:
            continue
        centre = offset + k * BIT_PERIOD
        lo = max(int(centre - full / 2), 0)
        hi = min(int(centre + full / 2) + 1, width)
        for i in range(lo, hi):
            acc[i] += delta * sine_squared_step((i - centre) / full + 0.5)
        for i in range(hi, width):
            acc[i] += delta
    return [max(0, min(255, int(LEVEL_LOW + a + 0.5))) for a in acc]


def self_test():
    """Render known waveforms and check the measurements recover them."""
    rep = Report()
    packet = [HAM_ENCODE[8], HAM_ENCODE[15], HAM_ENCODE[4], HAM_ENCODE[6]] + \
             [HAM_ENCODE[9]] * 6 + [0x55] + [0x20] * 31
    edge_packet = [0x00, 0xFF] * 21
    for offset, rise in ((6.0, 86.0), (5.62, 115.0), (6.0, 170.0)):
        lines = []
        for _ in range(8):
            for pkt in (packet, edge_packet):
                samples = render_line(pkt, offset, rise)
                sliced = slice_packet(samples)
                if sliced:
                    lines.append((samples, sliced))
        print('')
        print('synthetic: offset %.2f, rise %.0f ns' % (offset, rise))
        rep.check(len(lines) == 16, 'every synthetic line slices as WST (%d/16)' % len(lines))
        fits = [fit_bit_period(s, d) for s, d in lines]
        fits = [f for f in fits if f]
        period = statistics.mean(f[0] for f in fits)
        start = statistics.mean(f[1] for f in fits)
        rep.check(abs(period - BIT_PERIOD) < 0.0002,
                  'bit period recovered (%.5f vs %.5f)' % (period, BIT_PERIOD))
        rep.check(abs((start - period / 2) - offset) < 0.05,
                  'offset recovered (%.3f vs %.2f)' % (start - period / 2, offset))
        xs, ys = edge_profile(lines, True)
        fitted = fit_edge(xs, ys, True)
        sampled = ten_ninety(xs, ys, True)
        print('  rise %.0f ns as sampled, %.0f ns fitted, asked for %.0f'
              % (sampled, fitted, rise))
        rep.check(abs(fitted - rise) <= 8.0,
                  'edge fit recovers the rendered rise within 8 ns')
        rep.check(min(min(s) for s, _ in lines) >= LEVEL_LOW and
                  max(max(s) for s, _ in lines) <= LEVEL_HIGH,
                  'sine-squared render never leaves %d..%d' % (LEVEL_LOW, LEVEL_HIGH))
    print('')
    if rep.failures:
        print('SELF-TEST FAILED (%d):' % len(rep.failures))
        for text in rep.failures:
            print('  - %s' % text)
        return 1
    print('Self-test passed.')
    return 0


def analyse(path, max_edge_lines):
    frames = []
    edge_lines = []
    for index, frame in enumerate(parse_dump(path)):
        entry = {'frame': int(frame.get('Frame', -1)), 'lines': {}}
        for key, data in frame['lines'].items():
            if not data['luma']:
                continue
            samples = unhex(data['luma'])
            sliced = slice_packet(samples)
            entry['lines'][key] = {'samples': samples, 'sliced': sliced,
                                   'chroma': unhex(data['raw'])[0::2] if data['raw'] else []}
            if sliced and index < max_edge_lines:
                edge_lines.append((samples, sliced))
        frames.append(entry)
    return frames, edge_lines


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('capture', nargs='?', help='AJA frame dump to check')
    ap.add_argument('--self-test', action='store_true',
                    help='check the measurement code against synthetic waveforms')
    ap.add_argument('--expect-offset', type=float, default=None,
                    help='-teletext_vbi_offset the capture was made with')
    ap.add_argument('--offset-tol', type=float, default=0.15,
                    help='tolerance on the start offset, in samples (default 0.15)')
    ap.add_argument('--expect-rise-ns', type=float, default=None,
                    help='-teletext_rise_ns the capture was made with')
    ap.add_argument('--rise-tol-ns', type=float, default=15.0,
                    help='tolerance on the fitted edge, in ns (default 15)')
    ap.add_argument('--monotonic', action='store_true',
                    help='require that no sample leaves %d..%d' % (LEVEL_LOW, LEVEL_HIGH))
    ap.add_argument('--expect-header-attr', type=int, default=None,
                    help='spacing attribute every page header should lead with '
                         '(6 = Alpha Cyan, as Polistream)')
    ap.add_argument('--min-cleared-pct', type=float, default=None,
                    help='fail if fewer than this %% of captions get an erase '
                         'before the next caption replaces them')
    ap.add_argument('--expect-dual-field', action='store_true',
                    help='require ascending row order across the two fields')
    ap.add_argument('--expect-idl', action='store_true',
                    help='require the IDL filler continuity byte to step once per field')
    ap.add_argument('--edge-lines', type=int, default=200,
                    help='frames to use for the edge profile (default 200)')
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.capture:
        ap.error('a capture is required unless --self-test is given')

    frames, edge_lines = analyse(args.capture, args.edge_lines)
    rep = Report()
    rep.line('%s: %d frames' % (args.capture, len(frames)))

    # --- structure -------------------------------------------------------
    used = collections.Counter()
    undecoded = 0
    packets = 0
    for frame in frames:
        for key, data in frame['lines'].items():
            used[key] += 1
            packets += 1
            if data['sliced'] is None:
                undecoded += 1
    rep.line('')
    rep.line('Lines carrying data: %s' % ', '.join(
        'L%d F%d x%d' % (k[0], k[1], n) for k, n in sorted(used.items())))
    rep.check(undecoded == 0, 'every teletext line slices as WST (%d failed)' % undecoded)

    # --- packet integrity ------------------------------------------------
    ham_errors = 0
    parity_errors = 0
    rows = collections.Counter()
    charset = collections.Counter()
    for frame in frames:
        for data in frame['lines'].values():
            if not data['sliced']:
                continue
            packet = data['sliced']['bytes']
            row = row_address(packet)
            if row is None:
                ham_errors += 1
                continue
            rows[row] += 1
            if row == 0:
                if any(b not in HAM_DECODE for b in packet[5:13]):
                    ham_errors += 1
            elif row != 31:
                for byte in packet[5:45]:
                    if odd_parity(byte) == 0:
                        parity_errors += 1
                    charset[byte & 0x7F] += 1
    rep.line('')
    rep.line('Packets: %d   rows: %s' % (
        packets, ', '.join('r%d x%d' % (r, n) for r, n in sorted(rows.items()))))
    rep.check(ham_errors == 0, 'no Hamming 8/4 errors (%d)' % ham_errors)
    rep.check(parity_errors == 0, 'no odd-parity errors (%d)' % parity_errors)
    stray = sorted(c for c in charset if not 0x20 <= c < 0x7F and c not in (0x0A, 0x0B, 0x0D))
    rep.check(not stray, 'display bytes inside G0 (stray: %s)'
              % (' '.join('%02x' % c for c in stray) or 'none'))

    # --- levels ----------------------------------------------------------
    lows = collections.Counter()
    highs = collections.Counter()
    chroma = set()
    for frame in frames:
        for data in frame['lines'].values():
            if data['sliced']:
                lows[data['sliced']['low']] += 1
                highs[data['sliced']['high']] += 1
            chroma.update(data['chroma'])
    rep.line('')
    rep.line('Levels: min %s   max %s' % (
        ' '.join('%d(x%d)' % kv for kv in sorted(lows.items())),
        ' '.join('%d(x%d)' % kv for kv in sorted(highs.items()))))
    rep.check(chroma <= {128}, 'chroma neutral (saw %s)' % sorted(chroma))
    if args.monotonic:
        rep.check(min(lows) >= LEVEL_LOW and max(highs) <= LEVEL_HIGH,
                  'no sample outside %d..%d' % (LEVEL_LOW, LEVEL_HIGH))

    # --- clock and h-timing ----------------------------------------------
    periods, starts = [], []
    for samples, sliced in edge_lines:
        fit = fit_bit_period(samples, sliced)
        if fit:
            periods.append(fit[0])
            starts.append(fit[1])
    rep.line('')
    if periods:
        period = statistics.mean(periods)
        start = statistics.mean(starts)
        datum_us = (BT656_0H_OFFSET + start + PENULTIMATE_RUN_IN_BIT * period) / 13.5
        rep.line('Bit period %.5f samples -> %.1f kbit/s   (nominal %.5f / %.1f)'
                 % (period, SAMPLE_RATE_HZ / period / 1e3, BIT_PERIOD,
                    SAMPLE_RATE_HZ / BIT_PERIOD / 1e3))
        rep.line('Run-in bit 0 starts at sample %.3f (centre %.3f)'
                 % (start - period / 2, start))
        rep.line('OP-42 penultimate run-in "1" at %.3f us after 0H '
                 '(want %.1f +/- %.3f)' % (datum_us, OP42_DATUM_US, OP42_DATUM_TOL_US))
        rep.check(abs(period - BIT_PERIOD) < 0.0002,
                  'bit period is 72/37 samples (off by %.5f)' % (period - BIT_PERIOD))
        rep.check(abs(datum_us - OP42_DATUM_US) <= OP42_DATUM_TOL_US,
                  'h-timing inside the OP-42 window')
        if args.expect_offset is not None:
            got = start - period / 2
            rep.check(abs(got - args.expect_offset) <= args.offset_tol,
                      'run-in starts at the requested offset %.3f (measured %.3f)'
                      % (args.expect_offset, got))
    else:
        rep.check(False, 'enough transitions to fit the bit clock')

    # --- edge shape ------------------------------------------------------
    rep.line('')
    for rising in (True, False):
        name = 'rise' if rising else 'fall'
        xs, ys = edge_profile(edge_lines, rising)
        if not xs:
            rep.line('%s: no isolated transition to profile' % name)
            continue
        sampled = ten_ninety(xs, ys, rising)
        fitted = fit_edge(xs, ys, rising)
        rep.line('%s: %s ns as sampled, %.0f ns fitted   (profile %.1f..%.1f)'
                 % (name, '%.0f' % sampled if sampled else 'n/a', fitted,
                    min(ys), max(ys)))
        if rising and args.expect_rise_ns is not None:
            rep.check(abs(fitted - args.expect_rise_ns) <= args.rise_tol_ns,
                      'fitted edge matches the requested %.0f ns (measured %.0f)'
                      % (args.expect_rise_ns, fitted))

    # --- page headers ----------------------------------------------------
    leads = collections.Counter()
    pages = collections.Counter()
    p8ff_total = 0
    p8ff_spare = 0
    for frame in frames:
        f1 = frame['lines'].get((21, 1))
        f2 = frame['lines'].get((334, 2))
        for data in frame['lines'].values():
            if not data['sliced']:
                continue
            packet = data['sliced']['bytes']
            if row_address(packet) != 0:
                continue
            leads[packet[13]] += 1
            units = HAM_DECODE.get(packet[5])
            tens = HAM_DECODE.get(packet[6])
            if units is None or tens is None:
                continue
            pages['%X%X' % (tens, units)] += 1
        if f1 and f2 and f1['sliced'] and f2['sliced']:
            a, b = f1['sliced']['bytes'], f2['sliced']['bytes']
            if row_address(b) == 0 and HAM_DECODE.get(b[5]) == 15 \
                    and HAM_DECODE.get(b[6]) == 15:
                p8ff_total += 1
                if row_address(a) in (20, 22):
                    p8ff_spare += 1
    if leads:
        rep.line('')
        rep.line('Headers: %s' % ', '.join('P8%s x%d' % kv for kv in sorted(pages.items())))
        rep.line('Header display row leads with: %s'
                 % ', '.join('%02x x%d' % kv for kv in sorted(leads.items())))
        if p8ff_total:
            rep.line('P8FF in field 2: %d, of which %d follow a caption row on '
                     'field 1' % (p8ff_total, p8ff_spare))
        if args.expect_header_attr is not None:
            want = args.expect_header_attr & 0x1F
            if bin(want).count('1') % 2 == 0:
                want |= 0x80
            rep.check(set(leads) == {want},
                      'every header leads with attribute %02x (saw %s)'
                      % (want, ' '.join('%02x' % b for b in sorted(leads))))

    # --- caption lifetime -------------------------------------------------
    # A caption that is never erased stays on screen until the next one
    # overwrites it, so it has no end time. Walk the packets in transmission
    # order and ask, for each run of text rows, whether an erase header (C4=1)
    # arrives before the next text row does.
    # An erase that shares its frame with a caption row belongs to the caption
    # arriving, not the one leaving: it clears the page so the new text can be
    # written. Only a standalone erase, in a frame carrying no text at all, ends
    # a caption at its own end time. Counting the attached ones would score a
    # caption that lingered until it was overwritten as correctly cleared.
    timeline = []
    for frame in frames:
        packets = [d['sliced']['bytes'] for k, d in
                   sorted(frame['lines'].items(), key=lambda kv: kv[0][1])
                   if d['sliced']]
        has_text = any(row_address(p) in (18, 20, 22) for p in packets)
        for packet in packets:
            row = row_address(packet)
            if row in (18, 20, 22):
                timeline.append((frame['frame'], 'text', bytes(packet[5:45])))
            elif row == 0 and not has_text:
                units = HAM_DECODE.get(packet[8])   # S2 + C4 (erase page)
                if units is not None and units & 0x08:
                    timeline.append((frame['frame'], 'erase', None))
    captions = 0
    cleared = 0
    gaps = []
    last_text = None
    pending = False
    for frame_no, kind, payload in timeline:
        if kind == 'text':
            if payload != last_text:
                if pending:
                    captions += 1        # replaced without an erase
                if last_text is not None:
                    gaps.append(frame_no - last_seen)
                pending = True
                last_text = payload
            last_seen = frame_no
        elif kind == 'erase' and pending:
            captions += 1
            cleared += 1
            pending = False
            last_text = None
    if pending:
        captions += 1
    if captions:
        rep.line('')
        pct = 100.0 * cleared / captions
        rep.line('Captions: %d   ended by a standalone erase: %d (%.0f%%)'
                 % (captions, cleared, pct))
        if gaps:
            rep.line('Frames between captions: median %.0f, %d of %d closer than '
                     '8 frames' % (statistics.median(gaps),
                                   sum(1 for g in gaps if g < 8), len(gaps)))
        if args.min_cleared_pct is not None:
            rep.check(pct >= args.min_cleared_pct,
                      'at least %.0f%% of captions end with an erase (got %.0f%%)'
                      % (args.min_cleared_pct, pct))

    # --- per-field behaviour ---------------------------------------------
    rep.line('')
    deltas = collections.Counter()
    descending = 0
    ascending = 0
    for frame in frames:
        f1 = frame['lines'].get((21, 1))
        f2 = frame['lines'].get((334, 2))
        if not f1 or not f2 or not f1['sliced'] or not f2['sliced']:
            continue
        a, b = f1['sliced']['bytes'], f2['sliced']['bytes']
        ra, rb = row_address(a), row_address(b)
        if ra == 31 and rb == 31:
            deltas[(b[13] - a[13]) % 256] += 1
        elif ra and rb and ra != 31 and rb != 31 and ra != rb:
            if ra < rb:
                ascending += 1
            else:
                descending += 1
    if deltas:
        rep.line('IDL filler continuity, field 2 minus field 1: %s'
                 % ', '.join('+%d x%d' % kv for kv in deltas.most_common()))
        if args.expect_idl:
            rep.check(set(deltas) == {1},
                      'continuity byte steps once per field')
    if ascending or descending:
        rep.line('Two-row frames: %d ascending (20 then 22), %d descending'
                 % (ascending, descending))
        if args.expect_dual_field:
            rep.check(descending == 0, 'rows go out in ascending order')

    rep.line('')
    if rep.failures:
        rep.line('FAILED %d check(s):' % len(rep.failures))
        for text in rep.failures:
            rep.line('  - %s' % text)
        return 1
    rep.line('All checks passed.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
