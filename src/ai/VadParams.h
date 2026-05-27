// VadParams.h
// Default VAD + decoding parameters for whisper transcription.
//
// These defaults are taken from the ChickenRice / Faster-Whisper-TransWithAI
// community v1.5–v1.7 tuning notes — real-world tested on Chinese / Japanese
// dialogue audio for the subtitle-generation use case. We expose them as
// a struct so the future "AI 字幕 → 生成参数…" dialog can read/write them.
//
// References:
//   https://github.com/TransWithAI/Faster-Whisper-TransWithAI-ChickenRice
//   https://github.com/SYSTRAN/faster-whisper

#pragma once

namespace promp::ai {

struct VadDecodingParams {
    // ---- Silero VAD ----
    /// Speech-probability threshold. Higher = stricter (fewer false positives
    /// but missed soft speech). 0.5 = canonical Silero default for clean
    /// dialogue; 0.3 = recommended for ASMR / whispered / low-volume audio
    /// (which is what most "interesting" sources actually look like); 0.2 =
    /// very permissive, catches almost anything voiced.
    /// Tune up to 0.6 if you start getting false-positive segments on
    /// background music.
    float threshold = 0.20f;

    /// Minimum length a speech run must be to count as a segment.
    /// Filters out clicks / coughs / laughs.
    int   min_speech_duration_ms = 100;

    /// Silence longer than this ends a sentence. Smaller = chopper subtitles
    /// that refresh faster; larger = longer sentences but subtitles linger
    /// after speech ends.
    int   min_silence_duration_ms = 300;

    /// Padding around every detected speech run. Larger = subtitle visible
    /// longer (catches soft trailing consonants); smaller = crisper ends.
    int   speech_pad_ms = 200;

    /// Hard cap on a single VAD segment — prevents very long music / noise
    /// stretches from being mis-merged into one chunk.
    int   max_speech_duration_s = 30;

    // ---- Whisper decoding ----
    /// Stops the model emitting timestamps absurdly late at file start
    /// (typical of model failures). Larger value tolerates mid-stream
    /// audio (trimmed clips).
    float max_initial_timestamp_s = 30.0f;

    /// Initial decoding temperature. 0.0 = deterministic / greedy peak.
    /// When entropy_thold OR logprob_thold rejects the result, whisper
    /// retries with temperature += temperature_inc up to 1.0 — that
    /// fallback is what actually breaks repetition loops, so leave
    /// temperature_inc > 0 unless you really want deterministic output.
    float temperature       = 0.0f;
    float temperature_inc   = 0.2f;

    /// Compression-ratio-style threshold: whisper rejects a decoded
    /// segment whose entropy is *below* this value (highly repetitive
    /// output). Default whisper.cpp is 2.4 — we tighten to 2.6 because
    /// on Japanese ASMR / breathy audio the model loves to emit
    /// medium-entropy hallucination phrases (e.g. JR train names,
    /// stock NHK sentences) that just barely pass the default.
    float entropy_thold     = 2.6f;

    /// Average log-probability threshold. If avg_logprob < this AND
    /// no_speech_prob > no_speech_thold, the segment is treated as
    /// silence and dropped. Default -1.0; we use -0.7 so the model
    /// is stricter about low-confidence garbage.
    float logprob_thold     = -0.7f;

    /// No-speech probability threshold. Whisper marks a window as
    /// "no speech" when no_speech_prob > this. Default 0.6; we drop
    /// to 0.4 so silence / breathing / music gets rejected more
    /// aggressively. Lower = more false-negatives on quiet speech.
    float no_speech_thold   = 0.4f;

    /// Suppress non-speech tokens (laugh marks, brackets, music notes).
    /// whisper.cpp default is false; turning it on slightly reduces
    /// the model's willingness to emit canned NHK-style hallucinations
    /// on non-speech audio.
    bool  suppress_nst      = true;

    // ---- Anchor-mode VAD (timestamp correction only) --------------------
    /// In Anchor mode we run a SECOND VAD pass independently of the one
    /// used (if any) to filter audio. The anchor pass wants to detect
    /// "places where vocal activity starts" — and it should find enough
    /// of them to actually correct drift, but NOT so many that it
    /// over-anchors on every panting blip.
    ///
    float anchor_vad_threshold              = 0.20f;
    int   anchor_vad_min_speech_duration_ms = 100;
    int   anchor_vad_min_silence_duration_ms = 400;
    int   anchor_vad_speech_pad_ms          = 0;   // anchor wants the RAW onset

    /// Tolerance window for the snap-to-nearest-anchor step. A cue snaps
    /// to a VAD onset only if it falls within ±this of the cue's current
    /// (already-offset) start. Wider = catches bigger drifts, but a stray
    /// anchor can yank a cue off-position. 12s is a deliberate compromise
    /// — translation fine-tunes are known to drift 10–15s by mid-file.
    double anchor_tolerance_s = 12.0;

    // ---- Post-process anti-hallucination ----
    /// A phrase that repeats this many times *across the entire file*
    /// is treated as a hallucination and stripped — independent of
    /// whether the repeats are consecutive. The classic failure mode
    /// is the same line scattered across hundreds of windows.
    int   hallucination_min_repeats = 4;

    /// Additionally, the phrase's count must make up at least this
    /// fraction of all segments to be considered a hallucination
    /// (otherwise a real recurring lyric / catchphrase would be lost).
    double hallucination_fraction_thold = 0.25;

    // ---- Post-merge ----
    /// Adjacent segments with silence gap ≤ this are candidates to merge.
    int   merge_max_gap_ms = 1500;

    /// Stop merging when the merged subtitle's total length exceeds this.
    /// Tightened from the original 15000 so the merge pass no longer
    /// produces unreadable 12–20s super-segments on dense dialogue —
    /// the dedicated split pass below picks up whatever still slips
    /// through.
    int   merge_max_duration_ms = 6000;

    /// Drop the merge if its text is shorter than this many characters.
    int   merge_min_segment_chars = 10;

    bool  merge_enabled = true;

    // ---- Post-split (the inverse of merge) -------------------------------
    // After merging we sometimes still have lines that are too long for
    // comfortable reading — either because whisper emitted one big
    // segment, or because adjacent short fragments got chained right up
    // to the merge cap. This pass walks the post-merge list and breaks
    // anything over the budget at the most natural break-point
    // available (sentence punctuation > clause punctuation > space >
    // forced char-limit), distributing the original time window
    // proportionally to character count and respecting a per-chunk
    // minimum duration so subtitles never flash by faster than the eye
    // can track.
    bool  split_enabled = true;

    /// Soft maximum on characters per subtitle. The splitter aims to
    /// keep chunks ≤ this; only forced-break at this value when no
    /// good break-point exists in the buffer.
    int   split_max_chars = 28;

    /// Avoid producing chunks shorter than this many characters
    /// (unless they're a trailing remainder).
    int   split_min_chars = 6;

    /// Hard cap on per-chunk duration. If a cue is longer, we tighten
    /// the effective max_chars proportionally so the time gets carved
    /// into roughly this-sized slices. Matches merge_max_duration_ms.
    int   split_max_dur_ms = 6000;

    /// Minimum on-screen time per chunk. The allocator steals from the
    /// longest neighbour chunks to enforce this floor — guards against
    /// "subtitle flashed by, didn't read it" syndrome.
    int   split_min_dur_ms = 1200;

    /// Tiny gap inserted between adjacent split chunks so they don't
    /// visually run together in players that anti-flicker.
    int   split_gap_ms = 40;
};

} // namespace promp::ai
