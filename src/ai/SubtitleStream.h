// SubtitleStream.h
// Accumulates whisper segments and writes them to a .srt file on disk.
// Supports streaming/incremental updates: every flush() rewrites the
// entire file (cheap for typical subtitle sizes ≤ a few thousand lines).

#pragma once

#include "WhisperEngine.h"   // QList<WhisperSegment> needs the full type

#include <QString>
#include <QVector>

namespace promp::ai {

struct VadDecodingParams;

class SubtitleStream {
public:
    SubtitleStream() = default;
    explicit SubtitleStream(const QString& outputSrtPath);

    void setPath(const QString& p) { m_path = p; }
    [[nodiscard]] QString path() const noexcept { return m_path; }

    /// Append a segment (in arrival order — segments may overlap or be
    /// out-of-order; flush() sorts before writing).
    void append(const WhisperSegment& s);

    [[nodiscard]] int  size() const noexcept { return m_segs.size(); }
    [[nodiscard]] bool isEmpty() const noexcept { return m_segs.isEmpty(); }

    /// Collapse consecutive segments whose text is identical (the classic
    /// whisper "hallucination loop" on non-speech audio). The first instance
    /// is kept, its end-time extended to cover the run.
    /// Returns the number of dropped duplicates.
    int dedupeRepetitions();

    /// Scan the *entire* segment list for any single text-string that
    /// appears ≥ `minRepeats` times AND accounts for ≥ `fractionThold`
    /// of all segments — then drop every occurrence of that string.
    /// This catches the harder failure mode where whisper hallucinates
    /// the same canned phrase 200 times across the file but VAD chops
    /// it into non-consecutive segments so `dedupeRepetitions` misses it.
    /// Returns the number of dropped segments.
    int dropHallucinationPhrases(int minRepeats, double fractionThold);

    /// Drop segments whose duration exceeds `maxSec` (defaults to 30 s).
    /// After dedupeRepetitions(), any line that still spans more than this
    /// is almost certainly a hallucination loop the model failed to break
    /// out of — best to discard rather than show a stuck subtitle for
    /// minutes on end. Returns the number of dropped segments.
    int dropOverlongSegments(double maxSec = 30.0);

    /// Merge adjacent fragments per the rules in `p`. Mutates in place.
    void mergeSegments(const VadDecodingParams& p);

    /// Re-split any segment whose duration or text exceeds the
    /// `split_*` thresholds in `p`, picking break-points by punctuation
    /// preference (sentence > clause > whitespace > forced char-limit).
    /// Time is allocated proportionally to chunk length, then clamped
    /// to `p.split_min_dur_ms` per chunk by borrowing from the longest
    /// sibling. Returns the *net number of new segments* added (≥ 0).
    int splitLongSegments(const VadDecodingParams& p);

    /// Snap cue start-times to the nearest VAD speech onset, propagating
    /// the resulting offset to subsequent cues (so cumulative drift in
    /// translation fine-tunes is reset to zero at every anchor). A cue
    /// only gets anchored when there is a VAD speech onset within
    /// ±`tolerance_s` of its *current* (already-offset) start; otherwise
    /// it inherits the offset accumulated so far without further change.
    /// Each VAD onset anchors at most one cue (in chronological order).
    /// Returns the number of cues that were actually nudged.
    int anchorTimestamps(const std::vector<std::pair<double, double>>& vadRegions,
                         double tolerance_s);

    /// Sort + write the entire SRT to `path()`. Atomic via rename.
    bool flush(QString* errorOut = nullptr) const;

private:
    QString m_path;
    QVector<WhisperSegment> m_segs;
};

/// Format seconds as "hh:mm:ss,mmm" per SRT spec.
QString formatSrtTimestamp(double seconds);

} // namespace promp::ai
