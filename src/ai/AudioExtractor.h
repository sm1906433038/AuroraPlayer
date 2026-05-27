// AudioExtractor.h
// Pulls the audio track out of any video / audio file into a single mono
// 16 kHz s16le PCM .wav file on disk, suitable for whisper.cpp.
//
// The implementation creates its own short-lived libmpv handle in encode
// mode, isolated from the main player. This means we get the same wide
// codec support (mkv, mp4, hls, ...) without needing a separate ffmpeg.
//
// Usage is synchronous — call extract() from a worker thread.

#pragma once

#include <QString>
#include <atomic>
#include <functional>

namespace promp::ai {

struct AudioExtractionResult {
    bool    ok = false;
    QString wavPath;     ///< on-disk WAV (16 kHz, mono, s16le) on success
    QString error;       ///< human-readable error message
    QString diagnostics; ///< concatenated mpv log warnings/errors (always populated)
    double  durationSec = 0.0;
};

class AudioExtractor {
public:
    /// Progress callback: (current_seconds, total_seconds). total <= 0 means
    /// unknown. Return false from the callback to abort extraction.
    using ProgressFn = std::function<bool(double current, double total)>;

    /// Synchronously extract mono 16 kHz PCM WAV from `mediaPath` into
    /// a unique file under `tempDir`. The returned wavPath is owned by
    /// the caller (delete it when done).
    ///
    /// `audioTrackId`: pass 0 to keep mpv's auto-selected default track,
    /// or a positive integer to force a specific track id (matches the
    /// "aid" mpv property).
    static AudioExtractionResult extract(const QString&      mediaPath,
                                         const QString&      tempDir,
                                         int                 audioTrackId,
                                         ProgressFn          onProgress,
                                         std::atomic<bool>&  cancel);
};

/// Helper — load a 16 kHz mono s16le WAV into a float vector ([-1, 1]).
struct LoadedAudio {
    bool                 ok = false;
    std::vector<float>   samples;
    int                  sampleRate = 0;
    QString              error;
};
LoadedAudio loadMonoWav16k(const QString& wavPath);

} // namespace promp::ai
