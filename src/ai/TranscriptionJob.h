// TranscriptionJob.h
// Orchestrates extract → load WAV → whisper transcribe → write SRT, in
// a worker thread. Emits Qt signals so the UI thread can show progress
// and react to completion.

#pragma once

#include "VadParams.h"
#include "WhisperEngine.h"  // brings WhisperSegment in full so Q_DECLARE_METATYPE works

#include <QObject>
#include <QString>
#include <QThread>
#include <atomic>

namespace promp::ai {

class WhisperEngine;

struct TranscriptionConfig {
    QString  mediaPath;            ///< source video / audio path
    QString  modelPath;            ///< absolute path to ggml-*.bin
    QString  vadModelPath;         ///< absolute path to silero VAD ggml; empty = no VAD
    QString  outputSrtPath;        ///< where to write the final SRT
    QString  tempDir;              ///< for intermediate WAV
    QString  language = QStringLiteral("auto");
    bool     translateToEnglish = false;
    QString  initialPrompt;
    int      audioTrackId = 0;
    int      gpuDevice    = 0;
    int      threads      = 0;
    VadMode  vadMode      = VadMode::Filter;
    bool     vocalSeparation = false; ///< run BS-RoFormer vocal isolation before whisper
    VadDecodingParams vad;
};

class TranscriptionJob : public QObject {
    Q_OBJECT
public:
    explicit TranscriptionJob(TranscriptionConfig cfg, QObject* parent = nullptr);
    ~TranscriptionJob() override;

    /// Move the job into a fresh worker QThread and start it. Job auto-
    /// deletes itself when finished (use the signals; do not delete manually).
    void startAsync();

    /// Cooperative cancel — sets the flag, the worker steps stop checking
    /// at the next safe point and emit `finished(false, "Cancelled")`.
    void requestCancel();

signals:
    /// Multi-stage status text for the UI ("提取音频…", "加载模型…", "推理…").
    void stageChanged(const QString& msg);
    /// Progress 0–100 for the *current* stage.
    void progressChanged(int percent);
    /// A new segment was decoded — UI can preview it live.
    void newSegment(const promp::ai::WhisperSegment& seg);
    /// All done. `ok` reflects success; on failure `error` carries the why.
    /// `outputSrt` is the path written (may be partial on failure).
    void finished(bool ok, const QString& error, const QString& outputSrt);

private:
    Q_INVOKABLE void run();

    TranscriptionConfig m_cfg;
    std::atomic<bool>   m_cancel{false};
    QThread*            m_thread = nullptr;
};

} // namespace promp::ai
