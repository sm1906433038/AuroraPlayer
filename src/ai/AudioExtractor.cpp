#include "AudioExtractor.h"

#include <mpv/client.h>

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstring>
#include <vector>

namespace promp::ai {

// ---------------------------------------------------------------------------

namespace {

inline void setOpt(mpv_handle* m, const char* k, const char* v) {
    mpv_set_option_string(m, k, v);
}

} // namespace

AudioExtractionResult AudioExtractor::extract(const QString&     mediaPath,
                                              const QString&     tempDir,
                                              int                audioTrackId,
                                              ProgressFn         onProgress,
                                              std::atomic<bool>& cancel) {
    AudioExtractionResult r;

    if (!QFileInfo::exists(mediaPath)) {
        r.error = QStringLiteral("Source file does not exist: %1").arg(mediaPath);
        return r;
    }
    QDir().mkpath(tempDir);

    // Unique temp file name based on the input + timestamp so concurrent jobs
    // don't collide.
    const QString stem = QFileInfo(mediaPath).completeBaseName();
    const QString wavPath = QStringLiteral("%1/%2-%3.wav")
                                .arg(tempDir, stem,
                                     QString::number(QDateTime::currentMSecsSinceEpoch()));

    mpv_handle* m = mpv_create();
    if (!m) {
        r.error = QStringLiteral("mpv_create() failed for AudioExtractor");
        return r;
    }

    // --- Encode-mode setup. We disable video and route to a WAV writer.
    // The aformat audio filter forces a 16 kHz mono s16 PCM stream into
    // the encoder regardless of source layout; this is more reliable than
    // --audio-samplerate / --audio-channels, which target audio OUTPUT
    // devices and are ignored by some libmpv builds in encode mode.
    setOpt(m, "vo",                "null");
    setOpt(m, "video",             "no");
    setOpt(m, "hwdec",             "no");
    setOpt(m, "load-scripts",      "no");
    setOpt(m, "msg-level",         "all=warn");
    setOpt(m, "af",                "aformat=s16:16000:mono");
    setOpt(m, "of",                "wav");
    setOpt(m, "oac",               "pcm_s16le");
    setOpt(m, "o",                 wavPath.toUtf8().constData());

    // Pipe mpv warnings/errors through so we can see what's wrong.
    mpv_request_log_messages(m, "warn");

    if (audioTrackId > 0) {
        const QByteArray aid = QByteArray::number(audioTrackId);
        setOpt(m, "aid", aid.constData());
    }

    if (mpv_initialize(m) < 0) {
        mpv_destroy(m);
        r.error = QStringLiteral("mpv_initialize() failed for AudioExtractor");
        return r;
    }

    // Observe time-pos / duration for progress reporting.
    mpv_observe_property(m, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m, 0, "duration", MPV_FORMAT_DOUBLE);

    const QByteArray pathBytes = QDir::toNativeSeparators(mediaPath).toUtf8();
    const char* loadArgs[] = { "loadfile", pathBytes.constData(), nullptr };
    if (mpv_command(m, loadArgs) < 0) {
        mpv_terminate_destroy(m);
        QFile::remove(wavPath);
        r.error = QStringLiteral("mpv loadfile failed");
        return r;
    }

    double duration = 0.0;
    double curPos   = 0.0;
    bool   done     = false;
    bool   errored  = false;
    QString errMsg;
    QStringList diag;

    while (!done) {
        if (cancel.load()) {
            const char* quitArgs[] = {"quit", nullptr};
            mpv_command(m, quitArgs);
            errored = true;
            errMsg = QStringLiteral("Cancelled by user");
            break;
        }

        mpv_event* ev = mpv_wait_event(m, 0.25);
        switch (ev->event_id) {
        case MPV_EVENT_NONE: break;
        case MPV_EVENT_SHUTDOWN: done = true; break;
        case MPV_EVENT_LOG_MESSAGE: {
            auto* lm = static_cast<mpv_event_log_message*>(ev->data);
            if (lm) {
                const QString line = QStringLiteral("[%1/%2] %3")
                    .arg(QString::fromUtf8(lm->prefix),
                         QString::fromUtf8(lm->level),
                         QString::fromUtf8(lm->text).trimmed());
                qWarning().noquote() << "[mpv/extractor]" << line;
                if (diag.size() < 200) diag << line;
            }
            break;
        }
        case MPV_EVENT_END_FILE: {
            auto* end = static_cast<mpv_event_end_file*>(ev->data);
            if (end && end->reason != MPV_END_FILE_REASON_EOF
                    && end->reason != MPV_END_FILE_REASON_STOP) {
                errored = true;
                errMsg = QStringLiteral("mpv end_file reason=%1 error=%2")
                            .arg(int(end->reason)).arg(int(end->error));
            }
            done = true;
            break;
        }
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto* p = static_cast<mpv_event_property*>(ev->data);
            if (!p || p->format != MPV_FORMAT_DOUBLE) break;
            const double v = *static_cast<double*>(p->data);
            if (std::strcmp(p->name, "duration") == 0)  duration = v;
            else if (std::strcmp(p->name, "time-pos") == 0) curPos = v;
            if (onProgress && !onProgress(curPos, duration)) {
                cancel.store(true);
            }
            break;
        }
        default: break;
        }
    }

    mpv_terminate_destroy(m);

    r.diagnostics = diag.join(QChar('\n'));

    if (errored) {
        QFile::remove(wavPath);
        r.error = errMsg;
        return r;
    }
    const qint64 sz = QFileInfo(wavPath).size();
    if (!QFileInfo::exists(wavPath) || sz < 1024) {
        QFile::remove(wavPath);
        r.error = QStringLiteral(
            "WAV is empty (%1 bytes). Likely causes: video has no audio "
            "track, mpv lacks encode-mode support, or aformat filter failed.")
                    .arg(sz);
        return r;
    }

    r.ok = true;
    r.wavPath = wavPath;
    r.durationSec = duration;
    return r;
}

// ---------------------------------------------------------------------------
// Tiny WAV reader: handles canonical 16-bit / 16 kHz / mono PCM WAVs that
// mpv writes. Does not attempt to handle exotic formats.

LoadedAudio loadMonoWav16k(const QString& wavPath) {
    LoadedAudio out;
    QFile f(wavPath);
    if (!f.open(QIODevice::ReadOnly)) {
        out.error = QStringLiteral("Cannot open WAV: %1").arg(wavPath);
        return out;
    }
    QByteArray header = f.read(44);
    if (header.size() < 44 || std::memcmp(header.constData(), "RIFF", 4) != 0
                            || std::memcmp(header.constData() + 8, "WAVE", 4) != 0) {
        out.error = QStringLiteral("Not a RIFF/WAVE file: %1").arg(wavPath);
        return out;
    }
    // Walk chunks to find "fmt " and "data". We tolerate extra chunks.
    f.seek(12);
    int    sampleRate = 0;
    int    channels   = 0;
    int    bitsPerSample = 0;
    qint64 dataOffset = -1;
    qint64 dataSize   = 0;
    while (!f.atEnd()) {
        QByteArray cid = f.read(4);
        QByteArray sz  = f.read(4);
        if (cid.size() < 4 || sz.size() < 4) break;
        const quint32 chunkSize = quint8(sz[0])
                                 | (quint8(sz[1]) << 8)
                                 | (quint8(sz[2]) << 16)
                                 | (quint8(sz[3]) << 24);
        if (cid == "fmt ") {
            QByteArray fmt = f.read(chunkSize);
            if (fmt.size() < 16) { out.error = QStringLiteral("Truncated fmt chunk"); return out; }
            const quint16 audioFormat = quint8(fmt[0]) | (quint8(fmt[1]) << 8);
            channels      = quint8(fmt[2]) | (quint8(fmt[3]) << 8);
            sampleRate    = quint8(fmt[4]) | (quint8(fmt[5]) << 8)
                          | (quint8(fmt[6]) << 16) | (quint8(fmt[7]) << 24);
            bitsPerSample = quint8(fmt[14]) | (quint8(fmt[15]) << 8);
            if (audioFormat != 1) {
                out.error = QStringLiteral("WAV not s16 PCM (audioFormat=%1)").arg(audioFormat);
                return out;
            }
        } else if (cid == "data") {
            dataOffset = f.pos();
            dataSize   = chunkSize;
            break;
        } else {
            f.seek(f.pos() + chunkSize);
        }
    }

    if (dataOffset < 0 || sampleRate == 0 || channels == 0 || bitsPerSample != 16) {
        out.error = QStringLiteral("Unsupported WAV (sr=%1 ch=%2 bps=%3)")
                        .arg(sampleRate).arg(channels).arg(bitsPerSample);
        return out;
    }

    const qint64 frames = dataSize / (channels * (bitsPerSample / 8));
    out.samples.resize(static_cast<size_t>(frames));
    out.sampleRate = sampleRate;

    f.seek(dataOffset);
    QByteArray raw = f.read(dataSize);
    const qint16* p = reinterpret_cast<const qint16*>(raw.constData());
    if (channels == 1) {
        for (qint64 i = 0; i < frames; ++i)
            out.samples[i] = float(p[i]) / 32768.0f;
    } else {
        // Down-mix to mono by averaging channels (mpv should already give us
        // mono, but be safe).
        for (qint64 i = 0; i < frames; ++i) {
            int sum = 0;
            for (int c = 0; c < channels; ++c) sum += p[i * channels + c];
            out.samples[i] = float(sum) / (32768.0f * channels);
        }
    }

    out.ok = true;
    return out;
}

} // namespace promp::ai
