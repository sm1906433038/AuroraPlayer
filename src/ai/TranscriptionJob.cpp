#include "TranscriptionJob.h"

#include "AudioExtractor.h"
#include "SubtitleStream.h"
#include "WhisperEngine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaType>
#include <QProcess>
#include <QStringList>
#include <cmath>

namespace promp::ai {

TranscriptionJob::TranscriptionJob(TranscriptionConfig cfg, QObject* parent)
    : QObject(parent), m_cfg(std::move(cfg)) {
    qRegisterMetaType<promp::ai::WhisperSegment>("promp::ai::WhisperSegment");
}

TranscriptionJob::~TranscriptionJob() = default;

void TranscriptionJob::startAsync() {
    m_thread = new QThread;
    moveToThread(m_thread);

    connect(m_thread, &QThread::started, this, &TranscriptionJob::run);
    connect(this, &TranscriptionJob::finished, m_thread, &QThread::quit);
    connect(m_thread, &QThread::finished, this, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);

    m_thread->start();
}

void TranscriptionJob::requestCancel() {
    m_cancel.store(true);
}

void TranscriptionJob::run() {
    // ---- 1. Extract audio --------------------------------------------------
    emit stageChanged(tr("提取音频…"));
    emit progressChanged(0);

    AudioExtractor::ProgressFn extractProg = [this](double cur, double total) {
        if (m_cancel.load()) return false;
        if (total > 0.0) {
            int pct = qBound(0, int(cur * 100.0 / total), 100);
            emit progressChanged(pct);
        }
        return true;
    };

    auto extracted = AudioExtractor::extract(m_cfg.mediaPath,
                                             m_cfg.tempDir,
                                             m_cfg.audioTrackId,
                                             extractProg,
                                             m_cancel);
    // Surface mpv's own log lines to the UI so we can diagnose silent failures.
    if (!extracted.diagnostics.isEmpty()) {
        emit stageChanged(tr("提取诊断:\n%1").arg(extracted.diagnostics));
    }
    if (!extracted.ok) {
        emit finished(false,
                      tr("音频提取失败：%1\n--- mpv 日志 ---\n%2")
                          .arg(extracted.error, extracted.diagnostics),
                      m_cfg.outputSrtPath);
        return;
    }
    if (m_cancel.load()) {
        QFile::remove(extracted.wavPath);
        emit finished(false, tr("已取消"), m_cfg.outputSrtPath);
        return;
    }
    // Sanity check: how many samples did we actually read in?
    // (We log this in the UI so the user can see "0 samples" easily.)
    emit stageChanged(tr("提取完成: %1 (~%2 秒)")
                          .arg(QFileInfo(extracted.wavPath).fileName())
                          .arg(QString::number(extracted.durationSec, 'f', 1)));

    // ---- 1b. Vocal separation (optional) -----------------------------------
    // Runs BS-RoFormer via Python to isolate the vocal track, dramatically
    // improving whisper accuracy on music / background noise / breathy audio.
    // The separated vocals WAV replaces the raw WAV for all subsequent steps.
    QString wavToLoad = extracted.wavPath;
    if (m_cfg.vocalSeparation) {
        emit stageChanged(tr("人声分离中（BS-RoFormer）…"));
        emit progressChanged(0);

        // Locate the Python script and interpreter.
        const QString scriptDir = QCoreApplication::applicationDirPath()
                                   + QStringLiteral("/../../scripts");
        const QString script = QDir::cleanPath(scriptDir + QStringLiteral("/vocal-separator.py"));
        // Prefer the project's cached venv; fall back to system python.
        const QString venvPy = QDir::cleanPath(
            QCoreApplication::applicationDirPath()
            + QStringLiteral("/../../.cache/whisper-convert-venv/Scripts/python.exe"));
        const QString python = QFile::exists(venvPy) ? venvPy
                                                      : QStringLiteral("python");

        const QString sepOutDir = m_cfg.tempDir + QStringLiteral("/vocal-sep");
        QDir().mkpath(sepOutDir);

        QProcess proc;
        proc.setProcessChannelMode(QProcess::MergedChannels);
        QStringList args = {script, extracted.wavPath, sepOutDir,
                            QStringLiteral("--ensure-deps")};

        emit stageChanged(tr("人声分离中…（首次使用需下载模型 ~1.2 GB）"));
        proc.start(python, args);

        if (!proc.waitForStarted(30000)) {
            emit stageChanged(tr("⚠ 人声分离启动失败，跳过（%1）").arg(proc.errorString()));
        } else {
            // Stream output lines to the log while waiting. Track the
            // last non-empty line — the Python script prints the vocals
            // path as the very last line of stdout, which is our output.
            QString lastLine;
            while (proc.state() != QProcess::NotRunning) {
                if (m_cancel.load()) {
                    proc.kill();
                    proc.waitForFinished(5000);
                    QFile::remove(extracted.wavPath);
                    emit finished(false, tr("已取消"), m_cfg.outputSrtPath);
                    return;
                }
                proc.waitForReadyRead(2000);
                while (proc.canReadLine()) {
                    const QString line = QString::fromUtf8(proc.readLine()).trimmed();
                    if (!line.isEmpty()) {
                        emit stageChanged(line);
                        lastLine = line;
                    }
                }
            }
            // Drain anything left in the buffer after exit.
            const QByteArray tail = proc.readAll();
            if (!tail.isEmpty()) {
                for (const auto& l : QString::fromUtf8(tail).split(QLatin1Char('\n'))) {
                    const QString t = l.trimmed();
                    if (!t.isEmpty()) {
                        emit stageChanged(t);
                        lastLine = t;
                    }
                }
            }

            if (proc.exitCode() == 0 && !lastLine.isEmpty()) {
                // lastLine should be the absolute path to the vocals WAV.
                const QString vocalsPath = lastLine;
                if (QFile::exists(vocalsPath)) {
                    emit stageChanged(tr("人声分离完成: %1").arg(QFileInfo(vocalsPath).fileName()));
                    wavToLoad = vocalsPath;
                } else {
                    emit stageChanged(tr("⚠ 人声分离输出文件未找到: %1").arg(vocalsPath));
                }
            } else {
                emit stageChanged(tr("⚠ 人声分离失败 (exit %1)，使用原始音频")
                                      .arg(proc.exitCode()));
            }
        }
    }

    // ---- 2. Read WAV into float PCM ---------------------------------------
    emit stageChanged(tr("加载音频…"));
    auto loaded = loadMonoWav16k(wavToLoad);
    const qint64 wavSize = QFileInfo(wavToLoad).size();
    // Clean up both the original and the separated WAV (if different).
    QFile::remove(extracted.wavPath);
    if (wavToLoad != extracted.wavPath) QFile::remove(wavToLoad);
    if (!loaded.ok) {
        emit finished(false,
                      tr("音频解析失败：%1 (wav=%2 字节)")
                          .arg(loaded.error).arg(wavSize),
                      m_cfg.outputSrtPath);
        return;
    }
    emit stageChanged(tr("已加载 %1 个采样 (%2 秒 @ %3 Hz)")
                          .arg(loaded.samples.size())
                          .arg(QString::number(loaded.samples.size() / double(loaded.sampleRate), 'f', 1))
                          .arg(loaded.sampleRate));
    if (loaded.samples.empty()) {
        emit finished(false,
                      tr("WAV 中没有音频采样。可能：视频无音轨；或 mpv encode 模式未启用。"),
                      m_cfg.outputSrtPath);
        return;
    }

    // ---- 3. Load whisper model --------------------------------------------
    emit stageChanged(m_cfg.vadModelPath.isEmpty()
                          ? tr("加载模型…")
                          : tr("加载模型 + VAD…"));
    std::unique_ptr<WhisperEngine> engine;
    try {
        engine = std::make_unique<WhisperEngine>(m_cfg.modelPath,
                                                 m_cfg.gpuDevice,
                                                 m_cfg.vadModelPath);
    } catch (const std::exception& e) {
        emit finished(false,
                      tr("模型加载失败：%1").arg(QString::fromUtf8(e.what())),
                      m_cfg.outputSrtPath);
        return;
    }

    // ---- 4. Transcribe -----------------------------------------------------
    // Print the VAD knobs that will *actually* be used. The two modes read
    // from different fields of m_cfg.vad — printing the wrong field is
    // worse than printing nothing because it pretends a parameter was used
    // when it wasn't.
    if (!m_cfg.vadModelPath.isEmpty() && m_cfg.vadMode == VadMode::Filter) {
        emit stageChanged(tr("VAD 模式: 过滤音频 | "
                             "threshold=%1 min_speech=%2ms min_silence=%3ms "
                             "pad=%4ms max_speech=%5s")
                              .arg(QString::number(m_cfg.vad.threshold, 'f', 2))
                              .arg(m_cfg.vad.min_speech_duration_ms)
                              .arg(m_cfg.vad.min_silence_duration_ms)
                              .arg(m_cfg.vad.speech_pad_ms)
                              .arg(m_cfg.vad.max_speech_duration_s));
    } else if (!m_cfg.vadModelPath.isEmpty() && m_cfg.vadMode == VadMode::Anchor) {
        emit stageChanged(tr("VAD 模式: 时间戳校准 | "
                             "anchor_threshold=%1 anchor_min_speech=%2ms "
                             "anchor_min_silence=%3ms pad=%4ms tol=±%5s")
                              .arg(QString::number(m_cfg.vad.anchor_vad_threshold, 'f', 2))
                              .arg(m_cfg.vad.anchor_vad_min_speech_duration_ms)
                              .arg(m_cfg.vad.anchor_vad_min_silence_duration_ms)
                              .arg(m_cfg.vad.anchor_vad_speech_pad_ms)
                              .arg(QString::number(m_cfg.vad.anchor_tolerance_s, 'f', 1)));
    }
    emit stageChanged(engine->usingGpu() ? tr("AI 推理中（GPU）…")
                                          : tr("AI 推理中（CPU）…"));
    emit progressChanged(0);

    SubtitleStream stream(m_cfg.outputSrtPath);

    auto onSeg = [this, &stream](const WhisperSegment& s) {
        stream.append(s);
        // Best-effort live update — write current state to disk so the
        // user can already see partial subtitles in mpv if they hit "load".
        stream.flush();
        emit newSegment(s);
        return !m_cancel.load();
    };
    auto onProg = [this](int pct) {
        emit progressChanged(pct);
        return !m_cancel.load();
    };

    TranscriptionRequest req;
    req.samples            = &loaded.samples;
    req.sampleRate         = loaded.sampleRate;
    req.language           = m_cfg.language;
    req.translateToEnglish = m_cfg.translateToEnglish;
    req.initialPrompt      = m_cfg.initialPrompt;
    req.params             = &m_cfg.vad;
    req.vadMode            = m_cfg.vadModelPath.isEmpty()
                                ? VadMode::None
                                : m_cfg.vadMode;
    req.threads            = m_cfg.threads;

    auto tr_res = engine->transcribe(req, onSeg, onProg, m_cancel);
    if (!tr_res.diagnostics.isEmpty()) {
        emit stageChanged(tr_res.diagnostics);
    }

    if (!tr_res.ok) {
        emit finished(false, tr_res.error, m_cfg.outputSrtPath);
        return;
    }
    if (tr_res.segments.empty()) {
        emit finished(false,
                      tr("Whisper 返回成功但未产生任何字幕段。"
                         "常见原因：① 音频几乎是静音 / 纯音乐；"
                         "② 模型文件已损坏；③ 解码参数过于激进。"
                         "尝试换 small / medium 模型再试一次。"),
                      m_cfg.outputSrtPath);
        return;
    }
    emit stageChanged(tr("Whisper 完成：%1 段 (语言=%2)")
                          .arg(tr_res.segments.size())
                          .arg(tr_res.detectedLanguage));

    // ---- 5. Post-merge + final flush ---------------------------------------
    emit stageChanged(tr("整理字幕…"));
    const int before = stream.size();

    // Anchor pass FIRST — must run before dedupe / merge / split, because
    // anchoring shifts startSec and we want those shifted times to be the
    // basis of subsequent ordering decisions. Only applies in Anchor mode.
    int anchored = 0;
    if (m_cfg.vadMode == VadMode::Anchor && !tr_res.vadSpeechSegments.empty()) {
        // Diagnostic: dump the first handful of VAD anchor times so we can
        // tell from a paste-of-log whether the anchors are landing where we
        // think they should. Without this it's almost impossible to debug
        // why the snap missed.
        QStringList preview;
        const int previewN = qMin(int(tr_res.vadSpeechSegments.size()), 10);
        for (int i = 0; i < previewN; ++i) {
            const double t = tr_res.vadSpeechSegments[i].first;
            const int    s = int(t);
            preview << QString::asprintf("%02d:%02d.%01d",
                                          s / 60, s % 60,
                                          int(std::fmod(t, 1.0) * 10));
        }
        emit stageChanged(tr("VAD 锚点 (前 %1 个): %2%3")
                              .arg(previewN)
                              .arg(preview.join(QStringLiteral(", ")))
                              .arg(tr_res.vadSpeechSegments.size() > previewN
                                   ? QStringLiteral(" …") : QString()));

        anchored = stream.anchorTimestamps(tr_res.vadSpeechSegments,
                                           m_cfg.vad.anchor_tolerance_s);
        emit stageChanged(tr("时间戳校准: %1/%2 条字幕用 %3 个 VAD 锚点对齐 "
                             "(容忍 ±%4s)")
                              .arg(anchored)
                              .arg(stream.size())
                              .arg(int(tr_res.vadSpeechSegments.size()))
                              .arg(QString::number(m_cfg.vad.anchor_tolerance_s,
                                                   'f', 1)));
    }

    const int dupes  = stream.dedupeRepetitions();   // consecutive identical
    const int afterDedupe = stream.size();
    // After collapsing consecutive runs, scan globally for phrases that
    // dominate the file (the "scattered" hallucination case the user hit
    // even with VAD on).
    const int halluc = stream.dropHallucinationPhrases(
        m_cfg.vad.hallucination_min_repeats,
        m_cfg.vad.hallucination_fraction_thold);
    const int afterHalluc = stream.size();
    const int overlong = stream.dropOverlongSegments(45.0); // drop stuck lines
    const int afterDrop = stream.size();
    stream.mergeSegments(m_cfg.vad);
    const int afterMerge = stream.size();
    // Final readability pass: break any remaining long lines on
    // sentence/clause/whitespace boundaries so the user never sees
    // 13s subtitle slabs even when whisper itself emits them as one segment.
    const int splitAdded = stream.splitLongSegments(m_cfg.vad);
    emit stageChanged(tr("去重: %1 → %2 段 (-%3 连重)；"
                         "幻觉短句 -%4 → %5；超长段 -%6 → %7；"
                         "合并后 %8 段；拆分 +%9 → %10 段")
                          .arg(before).arg(afterDedupe).arg(dupes)
                          .arg(halluc).arg(afterHalluc)
                          .arg(overlong).arg(afterDrop)
                          .arg(afterMerge)
                          .arg(splitAdded).arg(stream.size()));
    QString err;
    if (!stream.flush(&err)) {
        emit finished(false, err, m_cfg.outputSrtPath);
        return;
    }

    emit progressChanged(100);
    emit finished(true, QString(), m_cfg.outputSrtPath);
}

} // namespace promp::ai
