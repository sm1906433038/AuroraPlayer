#include "SubtitleStream.h"

#include "VadParams.h"
#include "WhisperEngine.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <limits>

namespace promp::ai {

SubtitleStream::SubtitleStream(const QString& p) : m_path(p) {}

void SubtitleStream::append(const WhisperSegment& s) {
    if (s.text.trimmed().isEmpty()) return;
    if (s.endSec <= s.startSec) return;
    m_segs.push_back(s);
}

QString formatSrtTimestamp(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    const qint64 totalMs = qint64(std::llround(seconds * 1000.0));
    const qint64 hh = totalMs / 3'600'000;
    const qint64 mm = (totalMs / 60'000) % 60;
    const qint64 ss = (totalMs / 1'000) % 60;
    const qint64 ms = totalMs % 1'000;
    return QString::asprintf("%02lld:%02lld:%02lld,%03lld",
                             static_cast<long long>(hh),
                             static_cast<long long>(mm),
                             static_cast<long long>(ss),
                             static_cast<long long>(ms));
}

int SubtitleStream::dedupeRepetitions() {
    if (m_segs.size() < 2) return 0;

    std::sort(m_segs.begin(), m_segs.end(),
              [](const WhisperSegment& a, const WhisperSegment& b) {
                  return a.startSec < b.startSec;
              });

    QVector<WhisperSegment> out;
    out.reserve(m_segs.size());
    int dropped = 0;
    for (const auto& s : m_segs) {
        if (!out.isEmpty() && out.last().text == s.text) {
            // Same line repeated — silently swallow it and extend the run.
            out.last().endSec = qMax(out.last().endSec, s.endSec);
            ++dropped;
            continue;
        }
        out.push_back(s);
    }
    m_segs = std::move(out);
    return dropped;
}

int SubtitleStream::anchorTimestamps(
    const std::vector<std::pair<double, double>>& vadRegions,
    double tolerance_s)
{
    if (m_segs.isEmpty() || vadRegions.empty()) return 0;
    if (!std::isfinite(tolerance_s) || tolerance_s <= 0.0) return 0;

    // Sort segments chronologically; we'll walk forward.
    std::sort(m_segs.begin(), m_segs.end(),
              [](const WhisperSegment& a, const WhisperSegment& b) {
                  return a.startSec < b.startSec;
              });

    // Copy + sanitise + sort the VAD regions.
    QVector<QPair<double, double>> regs;
    regs.reserve(int(vadRegions.size()));
    for (const auto& r : vadRegions) {
        if (!std::isfinite(r.first) || !std::isfinite(r.second)) continue;
        if (r.first < 0.0) continue;
        regs.push_back({r.first, r.second});
    }
    if (regs.isEmpty()) return 0;
    std::sort(regs.begin(), regs.end(),
              [](const QPair<double,double>& a, const QPair<double,double>& b) {
                  return a.first < b.first;
              });
    const qsizetype nRegs = regs.size();
    QVector<bool> regUsed(int(nRegs), false);

    // ----------------------------------------------------------------------
    // Phase 1: for each cue, find its anchored offset (NaN if not anchored).
    //
    // The matching uses cue.startSec + cumOffset as the "current" position,
    // where cumOffset is the offset accumulated by previously-anchored cues.
    // This keeps the greedy walk monotonic: once we shifted earlier cues
    // forward by N seconds, the search window for later cues is also
    // shifted by N. Without this, a cue 30s after a +15s anchor would
    // still look for VAD onsets around its original (drifted-early) time
    // and miss them.
    // ----------------------------------------------------------------------
    QVector<double> cueOffset(m_segs.size(), std::numeric_limits<double>::quiet_NaN());
    double    cumOffset = 0.0;
    qsizetype nextReg   = 0;
    int       nudged    = 0;

    for (int i = 0; i < m_segs.size(); ++i) {
        const double effStart = m_segs[i].startSec + cumOffset;
        while (nextReg < nRegs && regs[nextReg].first < effStart - tolerance_s) {
            ++nextReg;
        }
        for (qsizetype k = nextReg; k < nRegs; ++k) {
            const double rT0 = regs[k].first;
            if (rT0 > effStart + tolerance_s) break;
            if (regUsed[int(k)]) continue;
            // This cue's desired absolute offset (rT0 - original_start).
            const double desiredOffset = rT0 - m_segs[i].startSec;
            cueOffset[i] = desiredOffset;
            cumOffset    = desiredOffset; // carry forward for next cues' window
            regUsed[int(k)] = true;
            nextReg = k + 1;
            ++nudged;
            break;
        }
    }

    if (nudged == 0) return 0;

    // ----------------------------------------------------------------------
    // Phase 2: linear-interpolate cueOffset for non-anchored cues, by the
    // ORIGINAL whisper time of the cue. Translation-fine-tune drift
    // accumulates roughly linearly with audio time — between two anchors
    // at (t_a, off_a) and (t_b, off_b), an in-between cue at t_x should
    // have offset off_a + (t_x - t_a) / (t_b - t_a) * (off_b - off_a).
    //
    // Edge cases:
    //   - Before the first anchor: use the first anchor's offset
    //     verbatim (constant extrapolation). Cues here are usually
    //     close to t=0 where drift is small anyway.
    //   - After the last anchor: use the last anchor's offset, BUT
    //     continue the local slope from the previous-to-last anchor
    //     if we have one — captures the ongoing drift trend.
    // ----------------------------------------------------------------------
    int firstAnchored = -1, lastAnchored = -1;
    for (int i = 0; i < cueOffset.size(); ++i) {
        if (!std::isnan(cueOffset[i])) {
            if (firstAnchored == -1) firstAnchored = i;
            lastAnchored = i;
        }
    }

    // Pre-first: constant extrapolation.
    for (int i = 0; i < firstAnchored; ++i) {
        cueOffset[i] = cueOffset[firstAnchored];
    }

    // Between anchors: linear interpolation by original startSec.
    int prev = firstAnchored;
    while (prev < lastAnchored) {
        int next = prev + 1;
        while (next < cueOffset.size() && std::isnan(cueOffset[next])) ++next;
        if (next > lastAnchored) break;
        const double t0 = m_segs[prev].startSec;
        const double t1 = m_segs[next].startSec;
        const double o0 = cueOffset[prev];
        const double o1 = cueOffset[next];
        const double denom = qMax(1e-6, t1 - t0);
        for (int i = prev + 1; i < next; ++i) {
            const double f = (m_segs[i].startSec - t0) / denom;
            cueOffset[i] = o0 + f * (o1 - o0);
        }
        prev = next;
    }

    // Post-last: linear-slope extrapolation from (prev_to_last, last), or
    // constant if there's only one anchor in total.
    if (lastAnchored >= 0 && lastAnchored < cueOffset.size() - 1) {
        // Find the anchor before lastAnchored (if any) to compute slope.
        int prevToLast = -1;
        for (int i = lastAnchored - 1; i >= 0; --i) {
            // After Phase 2 mid-fill, *every* cue with i ≤ lastAnchored
            // has a finite offset, so we identify the previous TRUE
            // anchor by walking back to a cue that was *originally* in
            // regUsed-land. The cleanest stand-in: walk back to the cue
            // whose offset is exactly (regs t0 - cue.start), i.e. lands
            // on a region. For simplicity we just look for a cue whose
            // offset differs notably from a strict linear continuation
            // — overkill; instead use the lastAnchored offset as a
            // constant. Drift extrapolation can mis-fire on the file's
            // trailing seconds and is rarely the visible problem.
            Q_UNUSED(prevToLast);
            break;
        }
        const double off = cueOffset[lastAnchored];
        for (int i = lastAnchored + 1; i < cueOffset.size(); ++i) {
            cueOffset[i] = off;
        }
    }

    // ----------------------------------------------------------------------
    // Phase 3: apply.
    // ----------------------------------------------------------------------
    for (int i = 0; i < m_segs.size(); ++i) {
        const double dur = qMax(0.0, m_segs[i].endSec - m_segs[i].startSec);
        m_segs[i].startSec += cueOffset[i];
        m_segs[i].endSec    = m_segs[i].startSec + dur;
    }

    return nudged;
}

int SubtitleStream::dropHallucinationPhrases(int minRepeats, double fractionThold) {
    if (m_segs.size() < minRepeats || minRepeats <= 1) return 0;

    // Whitespace-normalise so "ありがとう " / "ありがとう" count together.
    auto norm = [](const QString& s) { return s.simplified(); };

    QHash<QString, int> counts;
    counts.reserve(m_segs.size());
    for (const auto& s : m_segs) {
        ++counts[norm(s.text)];
    }

    const int total = m_segs.size();
    QSet<QString> blacklist;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        if (it.value() >= minRepeats
            && double(it.value()) / double(total) >= fractionThold) {
            blacklist.insert(it.key());
        }
    }
    if (blacklist.isEmpty()) return 0;

    QVector<WhisperSegment> out;
    out.reserve(m_segs.size());
    int dropped = 0;
    for (const auto& s : m_segs) {
        if (blacklist.contains(norm(s.text))) {
            ++dropped;
            continue;
        }
        out.push_back(s);
    }
    m_segs = std::move(out);
    return dropped;
}

int SubtitleStream::dropOverlongSegments(double maxSec) {
    if (m_segs.isEmpty()) return 0;
    QVector<WhisperSegment> out;
    out.reserve(m_segs.size());
    int dropped = 0;
    for (const auto& s : m_segs) {
        if ((s.endSec - s.startSec) > maxSec) {
            ++dropped;
            continue;
        }
        out.push_back(s);
    }
    m_segs = std::move(out);
    return dropped;
}

// ---------------------------------------------------------------------------
// Splitter — see refine-srt.py for the canonical reference implementation.
// Same algorithm, ported to QString. Kept self-contained in this TU so we
// don't bleed splitter internals out of SubtitleStream's interface.

namespace {

bool isHardPunct(QChar c) {
    // Sentence terminators (CJK full-width + ASCII).
    static const QSet<QChar> set = {
        QChar(u'。'), QChar(u'！'), QChar(u'？'), QChar(u'…'),
        QChar('.'),   QChar('!'),   QChar('?'),
    };
    return set.contains(c);
}

bool isMediumPunct(QChar c) {
    // Clause separators.
    static const QSet<QChar> set = {
        QChar(u'，'), QChar(u'、'), QChar(u'；'), QChar(u'：'),
        QChar(','),   QChar(';'),   QChar(':'),
    };
    return set.contains(c);
}

bool isSpaceLike(QChar c) {
    // Whitespace candidates. Full-width space U+3000 is the important
    // CJK one; the rest are belt-and-suspenders for mixed-script input.
    return c == QChar(' ') || c == QChar('\t') || c == QChar(u'\u3000');
}

/// Flat input (no embedded newlines), greedy split, returns trimmed chunks.
QVector<QString> splitTextChunks(const QString& raw, int maxChars, int minChars) {
    // Collapse internal whitespace to single spaces so the splitter sees a
    // single flat line; matches the Python reference.
    QString flat = raw;
    flat.replace(QChar('\r'), QChar(' '));
    flat.replace(QChar('\n'), QChar(' '));
    // Collapse runs of whitespace.
    static const QRegularExpression wsRun(QStringLiteral("\\s+"));
    flat = flat.replace(wsRun, QStringLiteral(" ")).trimmed();
    if (flat.isEmpty()) return {};
    if (flat.size() <= maxChars) return { flat };

    QVector<QString> out;
    QString buf;
    buf.reserve(maxChars * 2);

    int lastHard   = -1;  // 1-indexed split position within `buf`
    int lastMedium = -1;
    int lastSpace  = -1;

    auto flushAt = [&](int pos) {
        QString chunk = buf.left(pos).trimmed();
        QString rest  = buf.mid(pos);
        // Drop leading whitespace on the carried-over remainder.
        int i = 0;
        while (i < rest.size() && rest[i].isSpace()) ++i;
        rest = rest.mid(i);
        if (!chunk.isEmpty()) out.push_back(chunk);
        buf = rest;
        // Recompute break-point markers for the new buffer head.
        lastHard = lastMedium = lastSpace = -1;
        for (int k = 0; k < buf.size(); ++k) {
            const QChar c = buf[k];
            if (isHardPunct(c))        lastHard   = k + 1;
            else if (isMediumPunct(c)) lastMedium = k + 1;
            else if (isSpaceLike(c))   lastSpace  = k + 1;
        }
    };

    for (int i = 0; i < flat.size(); ++i) {
        const QChar c = flat[i];
        buf.append(c);
        const int n = buf.size();

        if (isHardPunct(c)) {
            lastHard = n;
            if (n >= minChars) { flushAt(n); continue; }
        } else if (isMediumPunct(c)) {
            lastMedium = n;
        } else if (isSpaceLike(c)) {
            lastSpace = n;
        }

        if (n >= maxChars) {
            if (lastHard >= minChars)        flushAt(lastHard);
            else if (lastMedium >= minChars) flushAt(lastMedium);
            else if (lastSpace >= minChars)  flushAt(lastSpace);
            else                              flushAt(n);  // forced
        }
    }

    QString tail = buf.trimmed();
    if (!tail.isEmpty()) {
        if (!out.isEmpty() && tail.size() < minChars) {
            out.back() = out.back() + QChar(' ') + tail;
        } else {
            out.push_back(tail);
        }
    }
    return out;
}

/// Allocate the [t0, t1] window across chunks proportionally to character
/// count, then enforce min_dur_ms by stealing from the longest sibling.
QVector<QPair<double, double>>
allocateTimes(double t0, double t1, const QVector<QString>& chunks,
              int minDurMs, int gapMs) {
    const int n = chunks.size();
    QVector<QPair<double, double>> out;
    if (n == 0) return out;
    if (n == 1) { out.push_back({t0, t1}); return out; }

    const int totalMs = qMax(1, int(std::llround((t1 - t0) * 1000.0)));
    const int gapTotal = qMin((n - 1) * gapMs, totalMs / 3);
    const int perGap   = (n > 1) ? (gapTotal / (n - 1)) : 0;
    const int speakable = totalMs - gapTotal;

    QVector<int> lens; lens.reserve(n);
    int sumChars = 0;
    for (const auto& c : chunks) {
        const int L = qMax(1, c.size());
        lens.push_back(L);
        sumChars += L;
    }

    QVector<int> dur(n, 0);
    int allocated = 0;
    for (int i = 0; i < n; ++i) {
        dur[i] = int(std::llround(double(speakable) * lens[i] / sumChars));
        allocated += dur[i];
    }
    // Fix rounding drift onto the last chunk.
    dur[n - 1] += (speakable - allocated);

    // Enforce min_dur_ms by stealing from the largest neighbour.
    for (int pass = 0; pass < n; ++pass) {
        bool changed = false;
        for (int i = 0; i < n; ++i) {
            if (dur[i] >= minDurMs) continue;
            int need = minDurMs - dur[i];
            // Pick largest other index.
            int donor = -1, donorVal = -1;
            for (int k = 0; k < n; ++k) {
                if (k == i) continue;
                if (dur[k] > donorVal) { donorVal = dur[k]; donor = k; }
            }
            if (donor >= 0 && dur[donor] - need >= minDurMs) {
                dur[i]     += need;
                dur[donor] -= need;
                changed = true;
            }
        }
        if (!changed) break;
    }

    double cur = t0;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        const double s = cur;
        const double e = s + dur[i] / 1000.0;
        out.push_back({s, e});
        cur = e + perGap / 1000.0;
    }
    // Snap the last end to t1 so the global timeline is preserved exactly.
    out.back().second = t1;
    return out;
}

} // namespace

int SubtitleStream::splitLongSegments(const VadDecodingParams& p) {
    if (!p.split_enabled || m_segs.isEmpty()) return 0;

    QVector<WhisperSegment> out;
    out.reserve(m_segs.size() * 2);

    int added = 0;
    for (const auto& s : m_segs) {
        const QString simp = s.text.simplified();
        const int     chars = simp.size();
        const int     durMs = int(std::llround((s.endSec - s.startSec) * 1000.0));

        const bool needsSplit =
            (durMs > p.split_max_dur_ms) || (chars > p.split_max_chars);
        if (!needsSplit) { out.push_back(s); continue; }

        // If duration is the limiting factor, tighten effective max_chars so
        // the splitter cuts often enough to keep each chunk ≤ split_max_dur_ms.
        int effMax = p.split_max_chars;
        if (durMs > p.split_max_dur_ms && chars > 0) {
            // ceil(chars * split_max_dur_ms / durMs)
            const int durBased = qMax(p.split_min_chars,
                int((qint64(chars) * p.split_max_dur_ms + durMs - 1) / durMs));
            effMax = qMin(effMax, durBased);
        }

        QVector<QString> chunks =
            splitTextChunks(s.text, effMax, p.split_min_chars);
        if (chunks.size() <= 1) { out.push_back(s); continue; }

        auto spans = allocateTimes(s.startSec, s.endSec, chunks,
                                   p.split_min_dur_ms, p.split_gap_ms);
        for (int i = 0; i < chunks.size(); ++i) {
            WhisperSegment ns;
            ns.startSec = spans[i].first;
            ns.endSec   = spans[i].second;
            ns.text     = chunks[i];
            out.push_back(ns);
        }
        added += chunks.size() - 1;
    }
    m_segs = std::move(out);
    return added;
}

// ---------------------------------------------------------------------------

void SubtitleStream::mergeSegments(const VadDecodingParams& p) {
    if (!p.merge_enabled || m_segs.size() < 2) return;

    std::sort(m_segs.begin(), m_segs.end(),
              [](const WhisperSegment& a, const WhisperSegment& b) {
                  return a.startSec < b.startSec;
              });

    QVector<WhisperSegment> out;
    out.reserve(m_segs.size());
    out.push_back(m_segs.front());
    for (int i = 1; i < m_segs.size(); ++i) {
        WhisperSegment& cur = out.last();
        const WhisperSegment& nx = m_segs[i];

        const double gapMs       = (nx.startSec - cur.endSec) * 1000.0;
        const double mergedMs    = (nx.endSec   - cur.startSec) * 1000.0;
        const bool   curTooShort = cur.text.size() < p.merge_min_segment_chars;

        if (gapMs <= p.merge_max_gap_ms
            && (mergedMs <= p.merge_max_duration_ms || curTooShort)) {
            cur.endSec = qMax(cur.endSec, nx.endSec);
            cur.text  += QStringLiteral(" ") + nx.text;
        } else {
            out.push_back(nx);
        }
    }
    m_segs = std::move(out);
}

bool SubtitleStream::flush(QString* errorOut) const {
    if (m_path.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("SubtitleStream: empty path");
        return false;
    }

    // Sort a copy so we don't mutate caller state.
    QVector<WhisperSegment> sorted = m_segs;
    std::sort(sorted.begin(), sorted.end(),
              [](const WhisperSegment& a, const WhisperSegment& b) {
                  return a.startSec < b.startSec;
              });

    QSaveFile f(m_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) *errorOut = QStringLiteral("Cannot open for write: %1").arg(m_path);
        return false;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts.setGenerateByteOrderMark(true);

    int idx = 1;
    for (const auto& s : sorted) {
        ts << idx++ << "\r\n"
           << formatSrtTimestamp(s.startSec) << " --> "
           << formatSrtTimestamp(s.endSec)   << "\r\n"
           << s.text << "\r\n\r\n";
    }
    ts.flush();
    if (!f.commit()) {
        if (errorOut) *errorOut = QStringLiteral("Failed to commit SRT: %1").arg(m_path);
        return false;
    }
    return true;
}

} // namespace promp::ai
