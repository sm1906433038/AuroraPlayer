#!/usr/bin/env python3
"""
refine-srt.py — split over-merged SRT subtitles into reader-friendly pieces.

Why this exists
---------------
AuroraPlayer's transcription pipeline runs a "merge adjacent short segments"
pass to clean up Whisper's chopped output. With the original
``merge_max_duration_ms = 15000`` default this is too aggressive on dense
dialogue: a single subtitle can end up 12–20 seconds long with 5–10 short
utterances jammed together separated by spaces. That's unreadable.

This tool re-splits any SRT into shorter chunks, while still respecting
a minimum on-screen duration so subtitles don't flash by faster than the
eye can track.

Splitting strategy (in priority order)
--------------------------------------
1. Hard sentence-ending punctuation: 。 ！ ？ . ! ? (and CJK variants)
2. Medium clause punctuation:        ， 、 ； ： , ; :
3. Whitespace runs (single/double space, full-width space)
4. Forced break at ``--max-chars`` if no better point is found

Each candidate position is graded; we then greedy-walk through the text
holding a buffer, and emit a chunk whenever:
- Buffer length is at least ``min_chars`` AND a higher-priority break is
  available, OR
- Buffer length exceeds ``max_chars`` and we fall back to the best break
  seen so far (even if soft) to avoid runaway lines.

Timing is distributed proportionally by character count, then clamped so
no chunk is shorter than ``min_dur_ms``. If forcing the minimum would
push a chunk past the parent's end time, neighbouring chunks shrink to
absorb it (preserving the original [t0, t1] window of the source line).

Usage
-----
    python refine-srt.py input.srt                  # writes input.refined.srt
    python refine-srt.py input.srt -o out.srt       # explicit output
    python refine-srt.py input.srt --in-place       # overwrite original (.bak kept)
    python refine-srt.py *.srt                      # batch (glob handled by shell)

Tuning knobs (with reasonable Chinese-subtitle defaults):
    --max-chars       28    Target maximum chars per line
    --min-chars        6    Don't split below this if avoidable
    --min-dur-ms    1200    Floor on per-chunk duration (ms)
    --max-dur-ms    6000    Above this, force a split even mid-clause
    --gap-ms          40    Tiny gap inserted between adjacent chunks
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

# ---------------------------------------------------------------------------
# SRT I/O — minimal, encoding-strict (UTF-8 with optional BOM in / UTF-8 BOM out).

# Cue index line + timestamp line + one-or-more text lines, separated by blanks.
_TS_RE = re.compile(
    r'(\d+):(\d{2}):(\d{2})[,.](\d{3})\s*-->\s*(\d+):(\d{2}):(\d{2})[,.](\d{3})'
)


@dataclass
class Cue:
    start_ms: int
    end_ms:   int
    text:     str   # may be multi-line; we collapse to single line on output

    @property
    def dur_ms(self) -> int:
        return self.end_ms - self.start_ms


def _ts_to_ms(h: str, m: str, s: str, ms: str) -> int:
    return ((int(h) * 60 + int(m)) * 60 + int(s)) * 1000 + int(ms)


def _ms_to_ts(ms: int) -> str:
    if ms < 0:
        ms = 0
    hh, rem = divmod(ms, 3_600_000)
    mm, rem = divmod(rem, 60_000)
    ss, mmm = divmod(rem, 1_000)
    return f"{hh:02d}:{mm:02d}:{ss:02d},{mmm:03d}"


def parse_srt(path: Path) -> list[Cue]:
    raw = path.read_bytes()
    # Strip BOM if present, then decode strict UTF-8.
    if raw.startswith(b'\xef\xbb\xbf'):
        raw = raw[3:]
    text = raw.decode('utf-8')

    cues: list[Cue] = []
    # SRT uses CRLF in the wild; normalise to LF, split into blocks.
    blocks = re.split(r'\r?\n\r?\n', text.replace('\r\n', '\n').strip())
    for block in blocks:
        lines = block.split('\n')
        if len(lines) < 2:
            continue
        # First line is the cue index — ignored, we re-number on output.
        # Find the timestamp line (sometimes preceded by the index, sometimes not).
        ts_line_idx = 0
        for i, ln in enumerate(lines):
            if _TS_RE.search(ln):
                ts_line_idx = i
                break
        else:
            continue
        m = _TS_RE.search(lines[ts_line_idx])
        if not m:
            continue
        t0 = _ts_to_ms(*m.group(1, 2, 3, 4))
        t1 = _ts_to_ms(*m.group(5, 6, 7, 8))
        body = '\n'.join(lines[ts_line_idx + 1:]).strip()
        if not body:
            continue
        cues.append(Cue(t0, t1, body))
    return cues


def write_srt(path: Path, cues: list[Cue]) -> None:
    # SRT spec is CRLF + UTF-8 with BOM is what most players (and our own
    # SubtitleStream.cpp) emit. Match that.
    out_lines: list[str] = []
    for i, c in enumerate(cues, 1):
        out_lines.append(str(i))
        out_lines.append(f"{_ms_to_ts(c.start_ms)} --> {_ms_to_ts(c.end_ms)}")
        out_lines.append(c.text)
        out_lines.append('')
    payload = '\r\n'.join(out_lines) + '\r\n'
    path.write_bytes(b'\xef\xbb\xbf' + payload.encode('utf-8'))


# ---------------------------------------------------------------------------
# Splitter.

# Punctuation priority. Higher = stronger break point. Keys are the chars
# that appear *just before* a candidate split position.
_HARD_PUNCT = set('。！？.!?…')      # sentence terminators
_MEDIUM_PUNCT = set('，、；：,;:')    # clause separators
_SPACE_CHARS  = set(' \t\u3000')     # half + full-width space (incl. tab)


def _visible_len(s: str) -> int:
    """Number of *visible* codepoints — strips leading/trailing whitespace."""
    return len(s.strip())


def _split_text(text: str, max_chars: int, min_chars: int) -> list[str]:
    """Greedy multi-level splitter. Returns a list of trimmed chunks."""
    # Collapse any internal newlines down to spaces so the splitter sees a
    # single flat string. We work on codepoints.
    flat = re.sub(r'\s+', ' ', text.replace('\n', ' ').replace('\r', ' ')).strip()
    if not flat:
        return []

    # If the whole thing is already short enough, return as-is.
    if len(flat) <= max_chars:
        return [flat]

    chunks: list[str] = []
    buf: list[str] = []
    # Track best fallback split points within `buf` (1-indexed, "split after
    # position k" means buf[:k] is chunk, buf[k:] is remainder).
    last_hard = -1
    last_medium = -1
    last_space = -1

    def flush_at(pos: int) -> None:
        nonlocal buf, last_hard, last_medium, last_space
        chunk = ''.join(buf[:pos]).strip()
        rest  = ''.join(buf[pos:]).lstrip()
        if chunk:
            chunks.append(chunk)
        buf = list(rest)
        # Re-scan rest for any latent break point that was past `pos`.
        last_hard = last_medium = last_space = -1
        for i, ch in enumerate(buf, 1):
            if ch in _HARD_PUNCT:
                last_hard = i
            elif ch in _MEDIUM_PUNCT:
                last_medium = i
            elif ch in _SPACE_CHARS:
                last_space = i

    for ch in flat:
        buf.append(ch)
        n = len(buf)

        if ch in _HARD_PUNCT:
            last_hard = n
            if n >= min_chars:
                flush_at(n)
                continue
        elif ch in _MEDIUM_PUNCT:
            last_medium = n
        elif ch in _SPACE_CHARS:
            last_space = n

        # Over the limit — split at best available point, or forced.
        if n >= max_chars:
            if last_hard >= min_chars:
                flush_at(last_hard)
            elif last_medium >= min_chars:
                flush_at(last_medium)
            elif last_space >= min_chars:
                flush_at(last_space)
            else:
                # No good split — accept oversized chunk only if there's
                # absolutely no break point. (Should be rare for natural text.)
                flush_at(n)

    if buf:
        tail = ''.join(buf).strip()
        if tail:
            # Merge into previous if tail is too short to stand alone.
            if chunks and len(tail) < min_chars:
                chunks[-1] = chunks[-1] + ' ' + tail
            else:
                chunks.append(tail)

    return chunks


def _allocate_times(t0: int, t1: int, chunks: list[str],
                    min_dur_ms: int, gap_ms: int) -> list[tuple[int, int]]:
    """Distribute the [t0, t1] window across chunks proportionally to text
    length, then clamp so no chunk is shorter than ``min_dur_ms``.

    Guarantees: chunks are contiguous (with optional ``gap_ms`` insertion
    between them, eaten back if doing so would push past t1) and the very
    first chunk starts at t0, the very last chunk ends at t1.
    """
    n = len(chunks)
    if n == 0:
        return []
    if n == 1:
        return [(t0, t1)]

    total_dur = max(1, t1 - t0)
    # Sum of inter-chunk gaps — never larger than 30% of total duration so
    # we don't starve the actual on-screen time.
    gap_total = min((n - 1) * gap_ms, total_dur // 3)
    per_chunk_gap = gap_total // max(1, n - 1)
    speakable = total_dur - gap_total

    char_lens = [max(1, len(c)) for c in chunks]
    sum_chars = sum(char_lens)
    raw = [int(round(speakable * cl / sum_chars)) for cl in char_lens]
    # Fix rounding drift so they sum exactly to `speakable`.
    drift = speakable - sum(raw)
    raw[-1] += drift

    # Apply min_dur floor by stealing from longer neighbours.
    # Simple two-pass: bump up short ones; then if total exceeds speakable
    # (it shouldn't, since we steal proportionally), trim the largest.
    for _ in range(n):
        changed = False
        for i, d in enumerate(raw):
            if d < min_dur_ms:
                need = min_dur_ms - d
                donor = max(range(n),
                            key=lambda k: raw[k] if k != i else -1)
                if raw[donor] - need >= min_dur_ms:
                    raw[i] += need
                    raw[donor] -= need
                    changed = True
        if not changed:
            break

    # Build absolute (start, end) tuples.
    out: list[tuple[int, int]] = []
    cur = t0
    for i, d in enumerate(raw):
        s = cur
        e = s + d
        out.append((s, e))
        cur = e + per_chunk_gap
    # Snap the very last end to t1 to absorb any leftover rounding.
    s0, _ = out[-1]
    out[-1] = (s0, t1)
    return out


def refine_cue(cue: Cue, max_chars: int, min_chars: int,
               min_dur_ms: int, max_dur_ms: int,
               gap_ms: int) -> list[Cue]:
    """Split a single cue if it is too long or has too much text.

    A cue can be "too long" in *either* dimension:
      - text length > max_chars  → split by char budget
      - duration   > max_dur_ms  → split even when text is short, because
        reading a 13-character line over 13 seconds is just dead screen time
    To handle the duration case we *tighten* max_chars when the duration
    pressure is the stronger of the two: an effective char budget that's
    proportional to the per-chunk duration goal. That way the same
    splitter gives us both behaviours from one pass.
    """
    chars = _visible_len(cue.text)
    needs_split = (cue.dur_ms > max_dur_ms) or (chars > max_chars)
    if not needs_split:
        return [cue]

    effective_max_chars = max_chars
    if cue.dur_ms > max_dur_ms and chars > 0:
        # Aim for at most max_dur_ms per chunk → at most this many chars.
        # Round up so we don't over-split on edge cases.
        dur_based = max(min_chars,
                        -(-chars * max_dur_ms // max(1, cue.dur_ms)))
        effective_max_chars = min(effective_max_chars, dur_based)

    chunks = _split_text(cue.text, max_chars=effective_max_chars, min_chars=min_chars)
    if len(chunks) <= 1:
        return [cue]

    spans = _allocate_times(cue.start_ms, cue.end_ms, chunks,
                            min_dur_ms=min_dur_ms, gap_ms=gap_ms)
    return [Cue(s, e, t) for (s, e), t in zip(spans, chunks)]


# ---------------------------------------------------------------------------

def _apply_time_adjust(cues: list[Cue], *,
                       time_shift_ms: int, time_stretch: float) -> None:
    """Shift and/or stretch all cue timestamps in place.

    Applied BEFORE splitting so the durations the splitter sees are
    already corrected.

    ``time_shift_ms``: added to every timestamp (positive = later).
        Use when subtitles appear N seconds too early — e.g.
        ``--time-shift 5000`` delays everything by 5 s.

    ``time_stretch``: multiplicative factor on every timestamp.
        Use when drift accumulates linearly — e.g. if the last
        subtitle is at 10:50 but the audio ends at 11:00,
        ``--time-stretch 1.015`` stretches proportionally.
    """
    for c in cues:
        c.start_ms = max(0, int(round(c.start_ms * time_stretch)) + time_shift_ms)
        c.end_ms   = max(c.start_ms + 1,
                         int(round(c.end_ms * time_stretch)) + time_shift_ms)


def refine_file(in_path: Path, out_path: Path, *,
                max_chars: int, min_chars: int,
                min_dur_ms: int, max_dur_ms: int, gap_ms: int,
                time_shift_ms: int = 0,
                time_stretch: float = 1.0) -> tuple[int, int]:
    cues = parse_srt(in_path)
    if not cues:
        return (0, 0)

    if time_shift_ms != 0 or time_stretch != 1.0:
        _apply_time_adjust(cues, time_shift_ms=time_shift_ms,
                           time_stretch=time_stretch)

    refined: list[Cue] = []
    for c in cues:
        refined.extend(
            refine_cue(c, max_chars=max_chars, min_chars=min_chars,
                       min_dur_ms=min_dur_ms, max_dur_ms=max_dur_ms,
                       gap_ms=gap_ms)
        )
    write_srt(out_path, refined)
    return (len(cues), len(refined))


def main(argv: Iterable[str]) -> int:
    ap = argparse.ArgumentParser(
        description="Re-split over-merged SRT subtitles into reader-friendly chunks.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument('inputs', nargs='+', help='SRT files to process')
    ap.add_argument('-o', '--output', help='Output path (single-file mode only)')
    ap.add_argument('--in-place', action='store_true',
                    help='Overwrite input; keep .bak backup alongside')
    ap.add_argument('--suffix', default='.refined',
                    help='Suffix inserted before .srt when no -o / --in-place')
    ap.add_argument('--max-chars', type=int, default=28,
                    help='Soft maximum characters per subtitle')
    ap.add_argument('--min-chars', type=int, default=6,
                    help='Avoid splits that produce chunks shorter than this')
    ap.add_argument('--min-dur-ms', type=int, default=1200,
                    help='Minimum on-screen duration per chunk (milliseconds)')
    ap.add_argument('--max-dur-ms', type=int, default=6000,
                    help='Above this, force a split even with weak break points')
    ap.add_argument('--gap-ms', type=int, default=40,
                    help='Tiny gap between adjacent chunks (milliseconds)')

    tg = ap.add_argument_group('time correction',
        'Fix timestamp drift from translation-style Whisper fine-tunes')
    tg.add_argument('--time-shift', type=int, default=0, metavar='MS',
                    help='Shift all timestamps by this many milliseconds '
                         '(positive = later; e.g. 5000 = delay 5s)')
    tg.add_argument('--time-stretch', type=float, default=1.0, metavar='FACTOR',
                    help='Multiply all timestamps by this factor '
                         '(e.g. 1.015 stretches 10:50 to ~11:00)')

    args = ap.parse_args(list(argv))

    if args.output and len(args.inputs) != 1:
        ap.error("-o is only valid with a single input file")

    for src in args.inputs:
        in_path = Path(src)
        if not in_path.exists():
            print(f"  [skip] {in_path} (not found)")
            continue

        if args.output:
            out_path = Path(args.output)
        elif args.in_place:
            bak = in_path.with_suffix(in_path.suffix + '.bak')
            if not bak.exists():
                bak.write_bytes(in_path.read_bytes())
            out_path = in_path
        else:
            out_path = in_path.with_name(in_path.stem + args.suffix + in_path.suffix)

        before, after = refine_file(
            in_path, out_path,
            max_chars=args.max_chars, min_chars=args.min_chars,
            min_dur_ms=args.min_dur_ms, max_dur_ms=args.max_dur_ms,
            gap_ms=args.gap_ms,
            time_shift_ms=args.time_shift,
            time_stretch=args.time_stretch,
        )
        if before == 0:
            print(f"  [empty] {in_path}")
            continue
        delta = after - before
        sign = '+' if delta >= 0 else ''
        print(f"  {in_path}")
        print(f"    -> {out_path}")
        print(f"    cues: {before} -> {after}  ({sign}{delta})")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
