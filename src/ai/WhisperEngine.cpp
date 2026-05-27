#include "WhisperEngine.h"

#include "VadParams.h"

#include "whisper.h"

#include <QByteArray>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <cstdint>
#include <stdexcept>

namespace {

// Reject files that obviously aren't ggml so we fail with a *useful* message
// rather than the opaque "whisper_init_from_file failed" you get otherwise.
//
// whisper.cpp uses `fwrite(&magic, sizeof(int), 1, f)` (or struct.pack("i", ...))
// which writes the int as native byte order — on every platform we ship to
// (x86 / ARM Windows/Linux/Mac) that is little-endian. So when we read the
// first 4 bytes as a uint32_t in LE we should see one of these values:
//   0x67676d6c  "ggml" (legacy, used by convert-h5-to-ggml.py)
//   0x67676d66  "ggmf"
//   0x67676a74  "ggjt"
//   0x46554747  "GGUF" (modern format, all new gguf files)
// On disk the bytes therefore appear *reversed* — e.g. ggml shows up as
// "6c 6d 67 67". Comparing buf[i] to ASCII letters is wrong; compare the
// reconstructed little-endian uint32 instead.
std::pair<bool, QString> sniffGgmlMagic(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {false, QStringLiteral("无法打开模型文件读取头部")};
    }
    unsigned char buf[4] = {};
    const qint64 n = f.read(reinterpret_cast<char*>(buf), 4);
    f.close();
    if (n != 4) {
        return {false, QStringLiteral("模型文件太小（不足 4 字节）")};
    }
    const uint32_t magic =
          uint32_t(buf[0])
        | (uint32_t(buf[1]) <<  8)
        | (uint32_t(buf[2]) << 16)
        | (uint32_t(buf[3]) << 24);
    switch (magic) {
        case 0x67676d6cu: // ggml
        case 0x67676d66u: // ggmf
        case 0x67676a74u: // ggjt
        case 0x46554747u: // GGUF
            return {true, QString()};
        default:
            break;
    }
    // Report both the on-disk byte order and the reconstructed magic so we
    // can diagnose new/unknown formats from the user log directly.
    const QString hex = QString::asprintf("%02x %02x %02x %02x",
        buf[0], buf[1], buf[2], buf[3]);
    return {false,
        QStringLiteral("不是 ggml/gguf 格式（文件头 = %1，magic = 0x%2）。\n"
                       "如果这是 HuggingFace 上的海南鸡原版 model.bin，"
                       "那是 CTranslate2 格式，whisper.cpp 不能直接读。\n"
                       "请先运行 scripts/convert-chickenrice.cmd "
                       "把 SafeTensors 版本转成 ggml 后再放进模型目录。")
            .arg(hex).arg(magic, 8, 16, QLatin1Char('0'))};
}

} // namespace

namespace promp::ai {

// ---------------------------------------------------------------------------
// PIMPL: hold the whisper_context plus the run-time cancel flag for the
// abort-callback bridge.

struct WhisperEngine::Impl {
    whisper_context* ctx = nullptr;
    bool usingGpu = false;
    QByteArray vadModelPathUtf8; ///< kept alive while transcribe() runs

    // Per-run state, captured by the C-style callbacks below.
    std::atomic<bool>*               cancelPtr = nullptr;
    WhisperEngine::SegmentFn         onSegment;
    WhisperEngine::ProgressFn        onProgress;
    TranscriptionResult              result; // accumulated segments
};

namespace {

bool abortCb(void* user) {
    auto* d = static_cast<WhisperEngine::Impl*>(user);
    return d && d->cancelPtr && d->cancelPtr->load();
}

void newSegmentCb(whisper_context* ctx, whisper_state* /*state*/, int n_new, void* user) {
    auto* d = static_cast<WhisperEngine::Impl*>(user);
    if (!d) return;
    const int total = whisper_full_n_segments(ctx);
    for (int i = total - n_new; i < total; ++i) {
        WhisperSegment seg;
        seg.startSec = whisper_full_get_segment_t0(ctx, i) / 100.0;
        seg.endSec   = whisper_full_get_segment_t1(ctx, i) / 100.0;
        seg.text     = QString::fromUtf8(whisper_full_get_segment_text(ctx, i)).trimmed();
        if (!seg.text.isEmpty()) {
            d->result.segments.push_back(seg);
            if (d->onSegment && !d->onSegment(seg)) {
                if (d->cancelPtr) d->cancelPtr->store(true);
            }
        }
    }
}

void progressCb(whisper_context* /*ctx*/, whisper_state* /*state*/, int progress, void* user) {
    auto* d = static_cast<WhisperEngine::Impl*>(user);
    if (!d || !d->onProgress) return;
    if (!d->onProgress(progress) && d->cancelPtr) {
        d->cancelPtr->store(true);
    }
}

} // namespace

// ---------------------------------------------------------------------------

WhisperEngine::WhisperEngine(const QString& modelPath, int gpuDevice,
                             const QString& vadModelPath)
    : m_d(std::make_unique<Impl>()), m_modelPath(modelPath) {
    m_d->vadModelPathUtf8 = vadModelPath.toUtf8();

    // Fail fast with a meaningful error if the file isn't ggml/gguf.
    if (auto [ok, err] = sniffGgmlMagic(modelPath); !ok) {
        throw std::runtime_error(err.toStdString());
    }
    if (!vadModelPath.isEmpty()) {
        if (auto [ok, err] = sniffGgmlMagic(vadModelPath); !ok) {
            throw std::runtime_error(("VAD 模型: " + err).toStdString());
        }
    }

    whisper_context_params cparams = whisper_context_default_params();
#ifdef GGML_USE_CUDA
    cparams.use_gpu    = (gpuDevice >= 0);
    cparams.gpu_device = (gpuDevice >= 0) ? gpuDevice : 0;
#else
    Q_UNUSED(gpuDevice);
    cparams.use_gpu = false;
#endif

    const QByteArray pathBytes = modelPath.toUtf8();
    m_d->ctx = whisper_init_from_file_with_params(pathBytes.constData(), cparams);

    if (!m_d->ctx) {
        // GPU init can fail (out-of-VRAM, driver) — retry on CPU before
        // giving up entirely.
        if (cparams.use_gpu) {
            cparams.use_gpu = false;
            m_d->ctx = whisper_init_from_file_with_params(pathBytes.constData(), cparams);
        }
        if (!m_d->ctx) {
            throw std::runtime_error("whisper_init_from_file failed: " + modelPath.toStdString());
        }
    }
    m_d->usingGpu = cparams.use_gpu;
}

WhisperEngine::~WhisperEngine() {
    if (m_d && m_d->ctx) {
        whisper_free(m_d->ctx);
        m_d->ctx = nullptr;
    }
}

bool WhisperEngine::usingGpu() const noexcept {
    return m_d && m_d->usingGpu;
}

// ---------------------------------------------------------------------------

TranscriptionResult WhisperEngine::transcribe(const TranscriptionRequest& req,
                                              SegmentFn  onSegment,
                                              ProgressFn onProgress,
                                              std::atomic<bool>& cancel) {
    TranscriptionResult res;
    if (!m_d || !m_d->ctx) {
        res.error = QStringLiteral("WhisperEngine: no context loaded");
        return res;
    }
    if (!req.samples || req.samples->empty()) {
        res.error = QStringLiteral("WhisperEngine: empty audio samples");
        return res;
    }
    if (req.sampleRate != WHISPER_SAMPLE_RATE) {
        res.error = QStringLiteral("WhisperEngine: sample rate must be %1 (got %2)")
                        .arg(WHISPER_SAMPLE_RATE).arg(req.sampleRate);
        return res;
    }

    // Reset PIMPL per-run state and bridge callbacks.
    m_d->cancelPtr  = &cancel;
    m_d->onSegment  = std::move(onSegment);
    m_d->onProgress = std::move(onProgress);
    m_d->result     = {};

    // Beam-search (vs greedy) is dramatically more robust against the
    // classic whisper failure mode where, on a stretch of breathing /
    // music / non-speech, the decoder falls into a fixed point and
    // re-emits the previous segment over and over for minutes.
    //
    // no_context=true is the *single most effective* knob for this —
    // it prevents the looped output from being fed back as context for
    // the next 30 s window, breaking the loop at the chunk boundary.
    whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    p.beam_search.beam_size = 5;
    p.no_context = true;

    p.print_progress   = false;
    p.print_realtime   = false;
    p.print_timestamps = false;
    p.print_special    = false;
    p.translate        = req.translateToEnglish;
    p.single_segment   = false;
    p.token_timestamps = false;

    p.detect_language  = (req.language.compare(QStringLiteral("auto"),
                                               Qt::CaseInsensitive) == 0);
    const QByteArray langBytes = req.language.toUtf8();
    p.language = p.detect_language ? "auto" : langBytes.constData();

    const QByteArray promptBytes = req.initialPrompt.toUtf8();
    if (!promptBytes.isEmpty()) p.initial_prompt = promptBytes.constData();

    p.n_threads = (req.threads > 0)
                    ? req.threads
                    : qMax(1, QThread::idealThreadCount() - 1);

    // ---- decoding tuning -------------------------------------------------
    // Apply the real whisper.cpp anti-hallucination knobs. The combo that
    // matters most on non-speech audio:
    //   no_speech_thold ↓  → "this window is silence" triggers easier
    //   logprob_thold   ↑  → low-confidence outputs get marked failed
    //   entropy_thold   ↑  → repetitive outputs trigger temperature fallback
    //   suppress_nst    ✓  → ban [laughter] / [music] / 「」-style tokens
    //   temperature_inc > 0 → fallback chain can escape repetition loops
    // Defaults in VadDecodingParams are tuned for the
    // "Japanese ASMR / non-speech-heavy" worst case; tweak in the future
    // params dialog for clean dialogue.
    const VadDecodingParams* vp = req.params;
    if (vp) {
        p.max_initial_ts  = vp->max_initial_timestamp_s;
        p.temperature     = vp->temperature;
        p.temperature_inc = vp->temperature_inc;
        p.entropy_thold   = vp->entropy_thold;
        p.logprob_thold   = vp->logprob_thold;
        p.no_speech_thold = vp->no_speech_thold;
        p.suppress_nst    = vp->suppress_nst;
    }
    // suppress_blank stays at its whisper.cpp default (true).

    // ---- Silero VAD integration ------------------------------------------
    // Three modes. Filter is whisper.cpp's classic "VAD filters the audio
    // before transcription"; Anchor runs whisper on the raw audio (Content
    // coverage like None) and we compute VAD segments separately after the
    // fact for the timestamp-anchor pass downstream; None disables VAD.
    const bool wantFilter = (req.vadMode == VadMode::Filter)
                            && !m_d->vadModelPathUtf8.isEmpty();
    if (wantFilter) {
        p.vad = true;
        p.vad_model_path = m_d->vadModelPathUtf8.constData();
        p.vad_params = whisper_vad_default_params();
        if (vp) {
            p.vad_params.threshold               = vp->threshold;
            p.vad_params.min_speech_duration_ms  = vp->min_speech_duration_ms;
            p.vad_params.min_silence_duration_ms = vp->min_silence_duration_ms;
            p.vad_params.speech_pad_ms           = vp->speech_pad_ms;
            p.vad_params.max_speech_duration_s   = float(vp->max_speech_duration_s);
        }
    }

    p.abort_callback           = &abortCb;
    p.abort_callback_user_data = m_d.get();
    p.new_segment_callback     = &newSegmentCb;
    p.new_segment_callback_user_data = m_d.get();
    p.progress_callback        = &progressCb;
    p.progress_callback_user_data = m_d.get();

    const int rc = whisper_full(m_d->ctx, p,
                                req.samples->data(),
                                int(req.samples->size()));
    if (rc != 0) {
        if (cancel.load()) {
            res.error = QStringLiteral("Cancelled");
        } else {
            res.error = QStringLiteral("whisper_full failed (rc=%1)").arg(rc);
        }
        m_d->cancelPtr = nullptr;
        return res;
    }

    // Authoritative enumeration: whatever the callbacks saw, trust the
    // post-call segment table directly. This decouples us from any version
    // skew or callback-misfire in the underlying whisper.cpp build.
    res.segments.clear();
    const int nSeg = whisper_full_n_segments(m_d->ctx);
    res.segments.reserve(nSeg);
    for (int i = 0; i < nSeg; ++i) {
        WhisperSegment seg;
        seg.startSec = whisper_full_get_segment_t0(m_d->ctx, i) / 100.0;
        seg.endSec   = whisper_full_get_segment_t1(m_d->ctx, i) / 100.0;
        seg.text     = QString::fromUtf8(
                          whisper_full_get_segment_text(m_d->ctx, i)).trimmed();
        if (!seg.text.isEmpty()) res.segments.push_back(seg);
    }

    res.ok = true;

    // ---- Anchor mode: run a *standalone* Silero VAD pass for timestamps --
    // In Anchor mode whisper has already seen the raw audio (good for
    // content coverage) but its output timestamps are subject to drift on
    // translation fine-tunes. We invoke Silero VAD directly here to get an
    // independent list of speech onsets — SubtitleStream::anchorTimestamps
    // uses these to snap each cue's start to the nearest detected onset,
    // resetting drift to zero at each anchor.
    if (req.vadMode == VadMode::Anchor && !m_d->vadModelPathUtf8.isEmpty()) {
        // CRITICAL: force the VAD context to CPU.
        //
        // The main whisper_context (m_d->ctx) is still alive at this point and
        // holds the CUDA backend. Spinning up a *second* CUDA backend for the
        // VAD model concurrently triggers a hard crash in ggml-cuda's global
        // state during teardown of the second one — observed as AuroraPlayer
        // exiting silently right after the SRT is written. The VAD network is
        // tiny (~1.8 MB), so CPU is plenty fast and avoids the conflict.
        //
        // Wrap the whole block in try / pointer-null defences too: an early
        // return on the VAD pass should NEVER abort the rest of the
        // transcription — we already have res.segments populated, the SRT
        // can ship without VAD anchors as a graceful fallback.
        try {
            whisper_vad_context_params vcparams = whisper_vad_default_context_params();
            vcparams.use_gpu    = false;
            vcparams.gpu_device = 0;
            vcparams.n_threads  = p.n_threads;
            whisper_vad_context* vctx = whisper_vad_init_from_file_with_params(
                m_d->vadModelPathUtf8.constData(), vcparams);
            if (vctx) {
                whisper_vad_params vparams = whisper_vad_default_params();
                // Anchor-mode VAD uses dedicated parameters, NOT the
                // user-facing sensitivity preset. The two have
                // contradictory goals:
                //   - Filter-mode VAD wants high recall on speech
                //     (low threshold → keep all soft speech in)
                //   - Anchor-mode VAD wants high precision on speech
                //     onsets (high threshold → don't anchor on
                //     panting / moaning blips)
                // The defaults in VadParams.h are tuned for the latter.
                if (vp) {
                    vparams.threshold               = vp->anchor_vad_threshold;
                    vparams.min_speech_duration_ms  = vp->anchor_vad_min_speech_duration_ms;
                    vparams.min_silence_duration_ms = vp->anchor_vad_min_silence_duration_ms;
                    vparams.speech_pad_ms           = vp->anchor_vad_speech_pad_ms;
                    vparams.max_speech_duration_s   = float(vp->max_speech_duration_s);
                }
                whisper_vad_segments* vsegs = whisper_vad_segments_from_samples(
                    vctx, vparams, req.samples->data(), int(req.samples->size()));
                if (vsegs) {
                    const int n = whisper_vad_segments_n_segments(vsegs);
                    res.vadSpeechSegments.reserve(n);
                    // CRITICAL: whisper_vad_segments_get_segment_t{0,1} return
                    // CENTISECONDS, not seconds. They're built via
                    // samples_to_cs() internally (samples / 16000 * 100). Same
                    // unit as whisper_full_get_segment_t0 — so we need the
                    // same /100.0 conversion. Forgetting this divisor stored
                    // anchor times 100× too large and made the snap pass miss
                    // every single cue, silently.
                    for (int i = 0; i < n; ++i) {
                        const double t0 = whisper_vad_segments_get_segment_t0(vsegs, i) / 100.0;
                        const double t1 = whisper_vad_segments_get_segment_t1(vsegs, i) / 100.0;
                        res.vadSpeechSegments.emplace_back(t0, t1);
                    }
                    whisper_vad_free_segments(vsegs);
                }
                whisper_vad_free(vctx);
            } else {
                qWarning() << "WhisperEngine: VAD anchor context init failed;"
                              " skipping timestamp anchoring";
            }
        } catch (const std::exception& e) {
            qWarning() << "WhisperEngine: VAD anchor pass threw:" << e.what()
                       << "— skipping timestamp anchoring";
            res.vadSpeechSegments.clear();
        } catch (...) {
            qWarning() << "WhisperEngine: VAD anchor pass threw (unknown);"
                          " skipping timestamp anchoring";
            res.vadSpeechSegments.clear();
        }
    }

    if (const int lid = whisper_full_lang_id(m_d->ctx); lid >= 0) {
        if (const char* lc = whisper_lang_str(lid)) {
            res.detectedLanguage = QString::fromLatin1(lc);
        }
    }

    // Stash a single-line diagnostic in detectedLanguage if empty so the
    // UI can show "lang=zh | whisper_n_seg=12 | kept=12" later.
    if (res.detectedLanguage.isEmpty()) {
        res.detectedLanguage = QStringLiteral("?n_seg=%1").arg(nSeg);
    }

    m_d->cancelPtr = nullptr;
    return res;
}

} // namespace promp::ai
