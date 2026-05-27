// TranslationJob.h
// Reads an SRT file, translates every cue's text via TranslationEngine,
// and writes one or two output SRT files:
//
//   * <stem>.<lang>.srt          - target-only ("zh-CN" / "en" etc.)
//   * <stem>.bilingual.srt       - target line on top, source on bottom
//
// Runs on a worker QThread; emits Qt signals so the UI can show progress.

#pragma once

#include "TranslationEngine.h"

#include <QObject>
#include <QString>
#include <QThread>
#include <atomic>

namespace promp::ai {

struct TranslationJobConfig {
    QString sourceSrtPath;
    QString outputTargetSrtPath;     ///< empty = auto-derive from source
    QString outputBilingualSrtPath;  ///< empty = skip bilingual output
    /// "above" = target on top + source below; "below" = swap.
    QString bilingualLayout = QStringLiteral("above");
    TranslationConfig engine;
};

class TranslationJob : public QObject {
    Q_OBJECT
public:
    explicit TranslationJob(TranslationJobConfig cfg, QObject* parent = nullptr);
    ~TranslationJob() override;

    void startAsync();
    void requestCancel();

signals:
    void stageChanged(const QString& msg);
    void progressChanged(int percent);           ///< 0..100
    /// Live per-line preview as translations come back.
    void linePreview(int idx, const QString& src, const QString& tgt);
    void finished(bool ok, const QString& error,
                  const QString& targetSrtPath,
                  const QString& bilingualSrtPath);

private:
    Q_INVOKABLE void run();

    TranslationJobConfig m_cfg;
    std::atomic<bool>    m_cancel{false};
    QThread*             m_thread = nullptr;
};

} // namespace promp::ai
