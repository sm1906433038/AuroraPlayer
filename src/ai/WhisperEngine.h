// WhisperEngine.h
// Thin wrapper around whisper.cpp's whisper_full() API.
//
// Each engine instance loads one ggml model into RAM/VRAM and keeps it
// resident; subsequent transcriptions on the same engine reuse the model.
// Models are large (~1 GB+), so the host application should keep one
// WhisperEngine alive across jobs rather than recreating it per file.
//
// Currently supports CPU and CUDA (if whisper.cpp is compiled with
// GGML_CUDA=ON). The implementation falls back to CPU automatically if
// CUDA initialisation fails.

#pragma once

#include <QMetaType>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace promp::ai {

struct VadDecodingParams;

struct WhisperSegment {
    double startSec = 0.0;
    double endSec   = 0.0;
    QString text;
};

/// What we do with the Silero VAD pre-stage. The three modes are deliberately
/// different and each fixes a different failure case:
///   None    — VAD never runs. Whisper sees the raw audio. Maximum content
///             coverage; vulnerable to translation-fine-tune timestamp drift
///             over long files and to hallucination loops on non-speech.
///   Filter  — whisper.cpp's own integrated VAD path. VAD chops the audio
///             into speech regions; whisper only runs on those regions and
///             output timestamps are mapped back via vad_mapping_table.
///             Best when there are large silent / music stretches AND the
///             content is unambiguously speech. Loses softly-voiced content
///             (whispers, breathy speech, moans-with-occasional-words).
///   Anchor  — whisper sees the raw audio (Content like None), but we ALSO
///             compute VAD speech regions separately and use them only as
///             *timestamp anchors* in post-processing. Each detected speech
///             onset resets the cumulative drift to zero for subsequent
///             cues. Recommended for translation-style fine-tunes
///             (ChickenRice) on soft-spoken content where Filter mode would
///             throw away half the dialogue.
enum class VadMode {
    None,
    Filter,
    Anchor,
};

struct TranscriptionRequest {
    /// 16 kHz mono float PCM in [-1, 1]. Required.
    const std::vector<float>* samples = nullptr;
    int sampleRate = 16000;

    /// "auto" to let whisper detect, or ISO code like "ja" / "zh" / "en".
    QString language = QStringLiteral("auto");

    /// If true, the model translates everything to English. Note: the
    /// ChickenRice model is a special case — it natively outputs Chinese
    /// for Japanese input, regardless of this flag. Keep this OFF for that.
    bool translateToEnglish = false;

    /// Optional: a hint prepended to the model's context to bias names /
    /// terms. Use sparingly — long prompts can degrade quality.
    QString initialPrompt;

    /// VAD + decoding hyper-parameters. Pass a non-null pointer to override.
    const VadDecodingParams* params = nullptr;

    /// How VAD interacts with the inference pass — see the VadMode enum above.
    /// Modes Filter and Anchor both require WhisperEngine to have been
    /// constructed with a non-empty `vadModelPath`; mode None ignores it.
    VadMode vadMode = VadMode::Filter;

    /// CPU threads. 0 = auto-detect.
    int threads = 0;
};

struct TranscriptionResult {
    bool ok = false;
    QString error;
    std::vector<WhisperSegment> segments;
    QString detectedLanguage;
    QString diagnostics; ///< human-readable extra info (VAD stats etc.)

    /// In Anchor mode, the VAD speech regions (t0, t1 in seconds) computed
    /// on the raw audio. Empty in any other mode. The downstream pipeline
    /// uses these as *timestamp anchors* — see SubtitleStream::anchorTimestamps.
    std::vector<std::pair<double, double>> vadSpeechSegments;
};

class WhisperEngine {
public:
    /// Construct + load. `gpuDevice` < 0 forces CPU; >= 0 selects CUDA
    /// device index (only honoured if whisper.cpp was built with CUDA).
    /// `vadModelPath` enables Silero VAD pre-filtering when non-empty —
    /// the path must point to a valid ggml VAD model file. Throws
    /// std::runtime_error on load failure.
    WhisperEngine(const QString& modelPath,
                  int gpuDevice = 0,
                  const QString& vadModelPath = {});
    ~WhisperEngine();

    WhisperEngine(const WhisperEngine&) = delete;
    WhisperEngine& operator=(const WhisperEngine&) = delete;

    /// Live segment callback: invoked as each segment is decoded.
    /// Return false to abort transcription early.
    using SegmentFn = std::function<bool(const WhisperSegment&)>;
    /// Coarse progress (0-100). Cooperative cancel: return false to abort.
    using ProgressFn = std::function<bool(int percent)>;

    /// Whether the engine is using a GPU backend.
    [[nodiscard]] bool usingGpu() const noexcept;

    [[nodiscard]] QString modelPath() const noexcept { return m_modelPath; }

    TranscriptionResult transcribe(const TranscriptionRequest& req,
                                   SegmentFn  onSegment,
                                   ProgressFn onProgress,
                                   std::atomic<bool>& cancel);

    /// Internal state. Declared public only so free-function C callbacks
    /// in WhisperEngine.cpp can name the type; do not access externally.
    struct Impl;

private:
    std::unique_ptr<Impl> m_d;
    QString m_modelPath;
};

} // namespace promp::ai

Q_DECLARE_METATYPE(promp::ai::WhisperSegment)
