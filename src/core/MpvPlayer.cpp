#include "MpvPlayer.h"

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QtGlobal>

#include <cmath>
#include <stdexcept>
#include <vector>

namespace promp {

// ---------- helpers ----------------------------------------------------------

namespace {

QVariant nodeToVariant(const mpv_node& node) {
    switch (node.format) {
        case MPV_FORMAT_STRING: return QString::fromUtf8(node.u.string);
        case MPV_FORMAT_FLAG:   return node.u.flag != 0;
        case MPV_FORMAT_INT64:  return QVariant::fromValue<qint64>(node.u.int64);
        case MPV_FORMAT_DOUBLE: return node.u.double_;
        default:                return {};
    }
}

} // namespace

// ---------- lifecycle --------------------------------------------------------

MpvPlayer::MpvPlayer(QObject* parent) : QObject(parent) {
    m_mpv = mpv_create();
    if (!m_mpv) {
        throw std::runtime_error("mpv_create() failed");
    }

    applyBaselineOptions();

    if (mpv_initialize(m_mpv) < 0) {
        mpv_destroy(m_mpv);
        m_mpv = nullptr;
        throw std::runtime_error("mpv_initialize() failed");
    }

    // Route only warnings+errors through the in-process event channel to keep
    // GUI thread responsive. The log-file already captures full verbose output.
    mpv_request_log_messages(m_mpv, "warn");

    // Bridge mpv wake-ups into the Qt event loop.
    connect(this, &MpvPlayer::wakeUp, this, &MpvPlayer::onWakeUp, Qt::QueuedConnection);
    mpv_set_wakeup_callback(m_mpv, &MpvPlayer::wakeupCb, this);

    registerObservers();

    // Apply the default (Standard) preset's runtime properties. MainWindow
    // will call setQualityPreset() right after construction to restore the
    // user's chosen level from QSettings if it differs.
    applyQualityPreset(m_qualityPreset);
}

MpvPlayer::~MpvPlayer() {
    destroyRenderContext();
    if (m_mpv) {
        mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
}

void MpvPlayer::applyBaselineOptions() {
    // These are the "best practical defaults" for a high-quality, low-effort player.
    // They are deliberately set BEFORE mpv_initialize().
    auto setOpt = [this](const char* name, const char* value) {
        if (mpv_set_option_string(m_mpv, name, value) < 0) {
            qWarning() << "mpv: failed to set option" << name << "=" << value;
        }
    };

    // ---- Output / decoding ------------------------------------------------
    setOpt("vo",                  "libmpv");
    setOpt("gpu-api",             "opengl");
    setOpt("video-sync",          "display-resample");

    // Hardware-decode pre-init seed (`gpu-hwdec-interop` is pre-init only;
    // `hwdec` itself is runtime-mutable). The user's choice is restored
    // from QSettings here so the very first frame uses the right pipeline.
    //
    // Note: gpu-hwdec-interop=auto in shinchiro's 2026-04 libmpv probes the
    // Vulkan interop too, which has wedged process startup when another
    // libmpv lives in-process. We pin to `d3d11va` explicitly to enable
    // zero-copy without that side effect.
    {
        QSettings s;
        const int raw = s.value(QStringLiteral("player/hwAccel"),
                                int(HwAccel::CopyMode)).toInt();
        switch (raw) {
        case int(HwAccel::Off):      m_hwAccel = HwAccel::Off;      break;
        case int(HwAccel::ZeroCopy): m_hwAccel = HwAccel::ZeroCopy; break;
        default:                     m_hwAccel = HwAccel::CopyMode; break;
        }
    }
    switch (m_hwAccel) {
    case HwAccel::Off:
        setOpt("hwdec",             "no");
        setOpt("gpu-hwdec-interop", "no");
        break;
    case HwAccel::CopyMode:
        setOpt("hwdec",             "d3d11va-copy,nvdec-copy,no");
        setOpt("gpu-hwdec-interop", "no");
        break;
    case HwAccel::ZeroCopy:
        setOpt("hwdec",             "d3d11va,nvdec,no");
        setOpt("gpu-hwdec-interop", "d3d11va");
        break;
    }

    // ---- Picture-quality pre-init seed -----------------------------------
    //
    // A subset of the picture-quality knobs (notably gpu-dumb-mode and
    // fbo-format) is pre-init-only in libmpv: mpv_set_property() accepts a
    // write at runtime but the GPU video renderer was already constructed,
    // so the actual shader / FBO path doesn't change until the vo is rebuilt.
    //
    // To make the "画质" menu's preset switch ACTUALLY visible without
    // having to recreate mpv from scratch, we read the user's saved preset
    // BEFORE mpv_initialize() and seed those pre-init properties to match.
    // applyQualityPreset() then handles all the runtime-mutable properties
    // and (when needed) triggers a current-file reload to flip dumb-mode.
    {
        QSettings s;
        const int rawPreset = s.value(QStringLiteral("player/qualityPreset"),
                                      int(QualityPreset::High)).toInt();
        switch (rawPreset) {
        case int(QualityPreset::Standard): m_qualityPreset = QualityPreset::Standard; break;
        case int(QualityPreset::Ultra):    m_qualityPreset = QualityPreset::Ultra;    break;
        case int(QualityPreset::Native):   m_qualityPreset = QualityPreset::Native;   break;
        default:                           m_qualityPreset = QualityPreset::High;     break;
        }
    }
    // Native and Standard both use dumb-mode (minimum shader path); High and
    // Ultra both use the full pipeline with rgba16f intermediate framebuffers.
    if (m_qualityPreset == QualityPreset::Standard ||
        m_qualityPreset == QualityPreset::Native) {
        setOpt("gpu-dumb-mode", "yes");
        setOpt("fbo-format",    "auto");
    } else {
        setOpt("gpu-dumb-mode", "no");
        setOpt("fbo-format",    "rgba16f");
    }

    setOpt("cache",               "yes");
    setOpt("demuxer-max-bytes",   "512MiB");
    setOpt("demuxer-max-back-bytes", "256MiB");

    setOpt("keep-open",           "yes");
    setOpt("idle",                "yes");
    setOpt("force-window",        "no");
    setOpt("osc",                 "no");
    setOpt("input-default-bindings", "no");
    setOpt("input-vo-keyboard",   "no");
    setOpt("ytdl",                "no");
    setOpt("sub-auto",            "fuzzy");
    setOpt("audio-file-auto",     "fuzzy");
    setOpt("screenshot-format",   "png");
    setOpt("screenshot-png-compression", "1");

    // ---- Subtitles (decent default look; user can override later) --------
    setOpt("sub-font",            "Microsoft YaHei UI");
    setOpt("sub-font-size",       "44");
    setOpt("sub-color",           "#FFFFFFFF");
    setOpt("sub-border-size",     "2.5");
    setOpt("sub-border-color",    "#FF000000");
    setOpt("sub-shadow-offset",   "1");
    setOpt("sub-shadow-color",    "#80000000");
    setOpt("sub-margin-y",        "40");
    // mpv auto-detects encoding but Chinese GBK / GB18030 subs often misfire;
    // we let it fall back to "auto" which uses uchardet internally.
    setOpt("sub-codepage",        "auto");
    setOpt("sub-ass-override",    "scale");

    // ---- Diagnostic log file ---------------------------------------------
    const QString logPath = QDir::temp().filePath("AuroraPlayer-mpv.log");
    setOpt("log-file",  logPath.toUtf8().constData());
    setOpt("msg-level", "all=info");
}

// ---------- Hardware acceleration --------------------------------------------

void MpvPlayer::setHwAccel(HwAccel a) {
    if (a == m_hwAccel) return;

    const bool oldNeedsInterop = (m_hwAccel == HwAccel::ZeroCopy);
    const bool newNeedsInterop = (a == HwAccel::ZeroCopy);
    const bool needReload = (oldNeedsInterop != newNeedsInterop);

    m_hwAccel = a;

    const char* hwdecVal = "no";
    switch (a) {
    case HwAccel::Off:      hwdecVal = "no";                          break;
    case HwAccel::CopyMode: hwdecVal = "d3d11va-copy,nvdec-copy,no";  break;
    case HwAccel::ZeroCopy: hwdecVal = "d3d11va,nvdec,no";            break;
    }
    if (m_mpv) mpv_set_property_string(m_mpv, "hwdec", hwdecVal);

    // Persist so the next launch primes the interop seed correctly.
    QSettings().setValue(QStringLiteral("player/hwAccel"), int(a));

    if (needReload && m_mpv) {
        mpv_node pathNode;
        if (mpv_get_property(m_mpv, "path", MPV_FORMAT_NODE, &pathNode) >= 0) {
            QString file;
            if (pathNode.format == MPV_FORMAT_STRING && pathNode.u.string)
                file = QString::fromUtf8(pathNode.u.string);
            mpv_free_node_contents(&pathNode);

            if (!file.isEmpty()) {
                double pos = 0.0;
                mpv_get_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
                const QByteArray fileUtf8 = file.toUtf8();
                const QByteArray startArg = QByteArray("start=") +
                                            QByteArray::number(pos, 'f', 3);
                const char* args[] = {
                    "loadfile", fileUtf8.constData(), "replace", "-1",
                    startArg.constData(), nullptr
                };
                mpv_command_async(m_mpv, 0, args);
                showText(tr("硬件加速已切换：重新加载以重建解码器"), 1800);
            }
        }
    }

    emit hwAccelChanged(a);
}

// ---------- Quality preset ---------------------------------------------------

void MpvPlayer::setQualityPreset(QualityPreset p) {
    if (p == m_qualityPreset) return;

    // Native and Standard both pin the pre-init pair (gpu-dumb-mode=yes,
    // fbo-format=auto); High and Ultra share the opposite pair. So a reload
    // is needed only when we cross that "dumb-mode group" boundary.
    auto isDumbGroup = [](QualityPreset q) {
        return q == QualityPreset::Standard || q == QualityPreset::Native;
    };
    const bool oldDumb = isDumbGroup(m_qualityPreset);
    const bool newDumb = isDumbGroup(p);
    const bool needReload = (oldDumb != newDumb);

    m_qualityPreset = p;
    applyQualityPreset(p);

    // Persist immediately so the next launch primes pre-init options correctly.
    QSettings().setValue(QStringLiteral("player/qualityPreset"), int(p));

    if (needReload && m_mpv) {
        // gpu-dumb-mode / fbo-format are pre-init only. Reload the current
        // file at the current position to rebuild the vo with the seeded
        // values written above (we'll re-seed on the NEXT app launch; for
        // the current session we also push them via set_property below so
        // the loadfile that follows picks them up).
        const char* dumb = newDumb ? "yes"  : "no";
        const char* fbo  = newDumb ? "auto" : "rgba16f";
        mpv_set_property_string(m_mpv, "gpu-dumb-mode", dumb);
        mpv_set_property_string(m_mpv, "fbo-format",    fbo);

        mpv_node pathNode;
        if (mpv_get_property(m_mpv, "path", MPV_FORMAT_NODE, &pathNode) >= 0) {
            QString file;
            if (pathNode.format == MPV_FORMAT_STRING && pathNode.u.string)
                file = QString::fromUtf8(pathNode.u.string);
            mpv_free_node_contents(&pathNode);

            if (!file.isEmpty()) {
                double pos = 0.0;
                mpv_get_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);

                const QByteArray fileUtf8  = file.toUtf8();
                const QByteArray startArg  = QByteArray("start=") +
                                             QByteArray::number(pos, 'f', 3);
                const char* args[] = {
                    "loadfile", fileUtf8.constData(), "replace", "-1",
                    startArg.constData(), nullptr
                };
                mpv_command_async(m_mpv, 0, args);
                showText(tr("画质已切换：重新加载以应用 GPU 渲染管线"), 1800);
            }
        }
    }

    emit qualityPresetChanged(p);
}

void MpvPlayer::applyQualityPreset(QualityPreset p) {
    if (!m_mpv) return;

    // mpv may not recognise every option on every build — we don't want one
    // unsupported key to abort the whole preset application. Log and continue.
    auto setProp = [this](const char* name, const char* value) {
        if (mpv_set_property_string(m_mpv, name, value) < 0) {
            qWarning() << "mpv: failed to set property" << name << "=" << value;
        }
    };

    // ---- Common to every preset (except Native) ----
    // We default the colour-management chain to the historical safe pair;
    // Native overrides both to "clip" further below for full passthrough.
    setProp("tone-mapping",         "bt.2446a");
    setProp("gamut-mapping-mode",   "perceptual");

    // NOTE: gpu-dumb-mode and fbo-format are deliberately NOT set here —
    // they are pre-init-only and seeded in applyBaselineOptions() from the
    // saved QSettings preset. setQualityPreset() does any reload needed
    // when those two need to change.
    switch (p) {
    case QualityPreset::Native: {
        // True passthrough: bilinear sampler, no colour science, no shaders,
        // no temporal work. Whatever the decoder hands us goes to the
        // display as-is (modulo whatever the display's own pipeline does).
        setProp("scale",                "bilinear");
        setProp("cscale",               "bilinear");
        setProp("dscale",               "bilinear");
        setProp("dither-depth",         "no");
        setProp("correct-downscaling",  "no");
        setProp("linear-downscaling",   "no");
        setProp("sigmoid-upscaling",    "no");
        setProp("deband",               "no");
        setProp("hdr-compute-peak",     "no");
        setProp("interpolation",        "no");
        setProp("tone-mapping",         "clip");
        setProp("gamut-mapping-mode",   "clip");
        // We don't clear sharpen/brightness/contrast/vf/glsl-shaders here —
        // those are user-managed via the dedicated dialogs. The radio name
        // in the menu makes the trade-off explicit.
        break;
    }
    case QualityPreset::Standard: {
        // Compatibility / fallback. Bare-minimum scalers, every advanced
        // feature off. Pairs with dumb-mode=yes seeded by baseline.
        setProp("scale",                "lanczos");
        setProp("cscale",               "lanczos");
        setProp("dscale",               "mitchell");
        setProp("dither-depth",         "auto");
        setProp("correct-downscaling",  "no");
        setProp("linear-downscaling",   "no");
        setProp("sigmoid-upscaling",    "no");
        setProp("deband",               "no");
        setProp("hdr-compute-peak",     "no");
        setProp("interpolation",        "no");
        break;
    }
    case QualityPreset::High: {
        // Daily-use sweet spot for a modern dGPU. Pairs with dumb-mode=no +
        // fbo-format=rgba16f seeded by baseline.
        setProp("scale",                "ewa_lanczossharp");
        setProp("cscale",               "spline36");
        setProp("dscale",               "mitchell");
        setProp("dither-depth",         "auto");
        setProp("correct-downscaling",  "yes");
        setProp("linear-downscaling",   "yes");
        setProp("sigmoid-upscaling",    "yes");
        setProp("deband",               "yes");
        setProp("deband-iterations",    "2");
        setProp("deband-threshold",     "48");
        setProp("deband-range",         "16");
        setProp("deband-grain",         "0");
        setProp("hdr-compute-peak",     "yes");
        setProp("interpolation",        "no");
        break;
    }
    case QualityPreset::Ultra: {
        // Everything in High, plus motion interpolation. video-sync is
        // display-resample (baseline) so 24p film maps cleanly onto a
        // 120/144/160 Hz display.
        setProp("scale",                "ewa_lanczossharp");
        setProp("cscale",               "spline36");
        setProp("dscale",               "mitchell");
        setProp("dither-depth",         "auto");
        setProp("correct-downscaling",  "yes");
        setProp("linear-downscaling",   "yes");
        setProp("sigmoid-upscaling",    "yes");
        setProp("deband",               "yes");
        setProp("deband-iterations",    "2");
        setProp("deband-threshold",     "48");
        setProp("deband-range",         "16");
        setProp("deband-grain",         "0");
        setProp("hdr-compute-peak",     "yes");
        setProp("interpolation",        "yes");
        setProp("tscale",               "oversample");
        setProp("scaler-resizes-only",  "no");
        break;
    }
    }

    // VR 高负载补偿（应用在 preset 之后，临时覆盖最昂贵的几项）。
    if (m_vrLoadMode) {
        setProp("scale",                "bilinear");
        setProp("cscale",               "bilinear");
        setProp("dscale",               "bilinear");
        setProp("correct-downscaling",  "no");
        setProp("linear-downscaling",   "no");
        setProp("sigmoid-upscaling",    "no");
        setProp("deband",               "no");
        setProp("hdr-compute-peak",     "no");
        setProp("interpolation",        "no");
    }
}

void MpvPlayer::setVrLoadMode(bool enabled) {
    if (enabled == m_vrLoadMode) return;
    m_vrLoadMode = enabled;
    // 重新应用当前预设；applyQualityPreset 会读 m_vrLoadMode 并按需覆盖。
    applyQualityPreset(m_qualityPreset);
}

void MpvPlayer::refreshTrackList() {
    m_tracks.clear();
    if (!m_mpv) { emit tracksChanged(); return; }

    mpv_node root;
    if (mpv_get_property(m_mpv, "track-list", MPV_FORMAT_NODE, &root) < 0) {
        emit tracksChanged();
        return;
    }
    if (root.format == MPV_FORMAT_NODE_ARRAY && root.u.list) {
        for (int i = 0; i < root.u.list->num; ++i) {
            const mpv_node& item = root.u.list->values[i];
            if (item.format != MPV_FORMAT_NODE_MAP || !item.u.list) continue;

            TrackInfo t;
            for (int j = 0; j < item.u.list->num; ++j) {
                const QByteArray  key = item.u.list->keys[j];
                const mpv_node&   v   = item.u.list->values[j];
                if      (key == "id"        && v.format == MPV_FORMAT_INT64)  t.id       = int(v.u.int64);
                else if (key == "type"      && v.format == MPV_FORMAT_STRING) {
                    const QByteArray s = v.u.string;
                    t.type = s == "audio"    ? TrackInfo::Type::Audio
                           : s == "sub"      ? TrackInfo::Type::Subtitle
                           : s == "video"    ? TrackInfo::Type::Video
                                             : TrackInfo::Type::Unknown;
                }
                else if (key == "title"     && v.format == MPV_FORMAT_STRING) t.title    = QString::fromUtf8(v.u.string);
                else if (key == "lang"      && v.format == MPV_FORMAT_STRING) t.lang     = QString::fromUtf8(v.u.string);
                else if (key == "codec"     && v.format == MPV_FORMAT_STRING) t.codec    = QString::fromUtf8(v.u.string);
                else if (key == "selected"  && v.format == MPV_FORMAT_FLAG)   t.selected = v.u.flag != 0;
                else if (key == "external"  && v.format == MPV_FORMAT_FLAG)   t.external = v.u.flag != 0;
                else if (key == "external-filename" && v.format == MPV_FORMAT_STRING)
                    t.filename = QString::fromUtf8(v.u.string);
            }
            if (t.id > 0 && t.type != TrackInfo::Type::Unknown)
                m_tracks.append(t);
        }
    }
    mpv_free_node_contents(&root);
    emit tracksChanged();
}

void MpvPlayer::registerObservers() {
    // Property IDs are just opaque uint64s — using 0 since we route by name.
    static const char* props[] = {
        "duration",
        "time-pos",
        "pause",
        "volume",
        "mute",
        "speed",
        "dwidth",
        "dheight",
        "media-title",
        "track-list",
        "eof-reached",
    };
    for (const char* p : props) {
        mpv_observe_property(m_mpv, 0, p, MPV_FORMAT_NODE);
    }
}

// ---------- render bridge ----------------------------------------------------

bool MpvPlayer::createRenderContext(void* (*get_proc_address)(void*, const char*),
                                    void* get_proc_address_ctx) {
    if (m_render) return true;
    if (!m_mpv)   return false;

    mpv_opengl_init_params gl_init{};
    gl_init.get_proc_address      = get_proc_address;
    gl_init.get_proc_address_ctx  = get_proc_address_ctx;

    mpv_render_param params[]{
        { MPV_RENDER_PARAM_API_TYPE,           const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL) },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init },
        { MPV_RENDER_PARAM_INVALID,            nullptr },
    };

    const int rc = mpv_render_context_create(&m_render, m_mpv, params);
    if (rc < 0) {
        m_render = nullptr;
        qWarning() << "mpv_render_context_create() failed with code" << rc;
        return false;
    }

    mpv_render_context_set_update_callback(m_render, &MpvPlayer::renderUpdateCb, this);
    return true;
}

void MpvPlayer::destroyRenderContext() {
    if (m_render) {
        mpv_render_context_set_update_callback(m_render, nullptr, nullptr);
        mpv_render_context_free(m_render);
        m_render = nullptr;
    }
}

// ---------- static callbacks -------------------------------------------------

void MpvPlayer::wakeupCb(void* ctx) noexcept {
    auto* self = static_cast<MpvPlayer*>(ctx);
    // Cross-thread: emit a queued signal so we drain events in the GUI thread.
    QMetaObject::invokeMethod(self, [self]() { emit self->wakeUp(); }, Qt::QueuedConnection);
}

void MpvPlayer::renderUpdateCb(void* ctx) noexcept {
    auto* self = static_cast<MpvPlayer*>(ctx);
    QMetaObject::invokeMethod(self,
        [self]() { emit self->renderUpdateRequested(); }, Qt::QueuedConnection);
}

// ---------- event loop drain -------------------------------------------------

void MpvPlayer::onWakeUp() {
    if (!m_mpv) return;

    for (;;) {
        mpv_event* ev = mpv_wait_event(m_mpv, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;

        switch (ev->event_id) {
            case MPV_EVENT_SHUTDOWN:
                return;

            case MPV_EVENT_LOG_MESSAGE: {
                auto* m = static_cast<mpv_event_log_message*>(ev->data);
                emit logMessage(QString::fromUtf8(m->level),
                                QString::fromUtf8(m->prefix),
                                QString::fromUtf8(m->text).trimmed());
                break;
            }

            case MPV_EVENT_FILE_LOADED: {
                QVariant path = getProperty("path");
                refreshTrackList();
                emit fileLoaded(path.toString());
                break;
            }

            case MPV_EVENT_END_FILE: {
                auto* e = static_cast<mpv_event_end_file*>(ev->data);
                emit endFile(static_cast<int>(e->reason));
                break;
            }

            case MPV_EVENT_PROPERTY_CHANGE: {
                auto* prop = static_cast<mpv_event_property*>(ev->data);
                if (!prop || !prop->data) break;
                const QByteArray name(prop->name);
                if (prop->format != MPV_FORMAT_NODE) break;
                const QVariant val = nodeToVariant(*static_cast<mpv_node*>(prop->data));

                if      (name == "duration")     emit durationChanged(val.toDouble());
                else if (name == "time-pos")     emit positionChanged(val.toDouble());
                else if (name == "pause")        emit pausedChanged(val.toBool());
                else if (name == "volume")       emit volumeChanged(val.toDouble());
                else if (name == "mute")         emit muteChanged(val.toBool());
                else if (name == "speed")        emit speedChanged(val.toDouble());
                else if (name == "media-title")  emit mediaTitleChanged(val.toString());
                else if (name == "track-list")   refreshTrackList();
                else if (name == "dwidth" || name == "dheight") {
                    QSize sz(getProperty("dwidth").toInt(),
                             getProperty("dheight").toInt());
                    if (sz.isValid()) emit videoSizeChanged(sz);
                }
                break;
            }

            default:
                break;
        }
    }
}

// ---------- commands ---------------------------------------------------------

bool MpvPlayer::loadFile(const QString& path, bool append) {
    const QByteArray u8 = path.toUtf8();
    const char* args[] = { "loadfile", u8.constData(), append ? "append-play" : "replace", nullptr };
    return mpv_command_async(m_mpv, 0, args) >= 0;
}

void MpvPlayer::play()        { setProperty("pause", false); }
void MpvPlayer::pause()       { setProperty("pause", true); }
void MpvPlayer::togglePause() { sendCommand({ "cycle", "pause" }); }
void MpvPlayer::stop()        { sendCommand({ "stop" }); }

void MpvPlayer::seekAbsolute(double seconds, bool exact) {
    sendCommand({ "seek", QString::number(seconds, 'f', 3),
                  exact ? "absolute+exact" : "absolute+keyframes" });
}
void MpvPlayer::seekRelative(double seconds, bool exact) {
    sendCommand({ "seek", QString::number(seconds, 'f', 3),
                  exact ? "relative+exact" : "relative+keyframes" });
}

void MpvPlayer::setSpeed(double speed)    { setProperty("speed",  speed); }
void MpvPlayer::setVolume(double percent) { setProperty("volume", percent); }
void MpvPlayer::setMute(bool muted)       { setProperty("mute",   muted); }

void MpvPlayer::screenshot(const QString& path) {
    if (path.isEmpty())
        sendCommand({ "screenshot", "video" });
    else
        sendCommand({ "screenshot-to-file", path, "video" });
}

// ---------- fine controls ---------------------------------------------------

void MpvPlayer::frameStep()     { sendCommand({ "frame-step" }); }
void MpvPlayer::frameBackStep() { sendCommand({ "frame-back-step" }); }

void MpvPlayer::stepSpeed(double delta) {
    double cur = getProperty("speed").toDouble();
    if (!std::isfinite(cur) || cur <= 0) cur = 1.0;
    double next = cur + delta;
    if (next < 0.05) next = 0.05;
    if (next > 10.0) next = 10.0;
    setSpeed(next);
    showText(QString("播放速度：%1×").arg(next, 0, 'f', 2));
}

void MpvPlayer::resetSpeed() {
    setSpeed(1.0);
    showText(QStringLiteral("播放速度：1.00×"));
}

void MpvPlayer::cycleAudio(int dir) {
    sendCommand({ "cycle", "audio", dir >= 0 ? "up" : "down" });
}

void MpvPlayer::cycleSubtitle(int dir) {
    sendCommand({ "cycle", "sub", dir >= 0 ? "up" : "down" });
}

void MpvPlayer::toggleSubtitleVisibility() {
    sendCommand({ "cycle", "sub-visibility" });
}

void MpvPlayer::setSubtitleDelay(double seconds) {
    setProperty("sub-delay", seconds);
    showText(QString("字幕延时：%1 秒").arg(seconds, 0, 'f', 3));
}

void MpvPlayer::setAudioDelay(double seconds) {
    setProperty("audio-delay", seconds);
    showText(QString("音频延时：%1 秒").arg(seconds, 0, 'f', 3));
}

// ---------- track switching -------------------------------------------------

void MpvPlayer::setAudioTrack(int id) {
    if (id <= 0) setProperty("aid", QString("no"));
    else         setProperty("aid", id);
}
void MpvPlayer::setSubtitleTrack(int id) {
    if (id <= 0) setProperty("sid", QString("no"));
    else         setProperty("sid", id);
}
void MpvPlayer::setVideoTrack(int id) {
    if (id <= 0) setProperty("vid", QString("no"));
    else         setProperty("vid", id);
}

// ---------- OSD --------------------------------------------------------------

void MpvPlayer::showText(const QString& msg, int durationMs) {
    sendCommand({ "show-text", msg, QString::number(durationMs) });
}

// ---------- generic property / command bridge --------------------------------

void MpvPlayer::setProperty(const QByteArray& name, const QVariant& value) {
    if (!m_mpv) return;

    switch (value.typeId()) {
        case QMetaType::Bool: {
            int v = value.toBool() ? 1 : 0;
            mpv_set_property(m_mpv, name.constData(), MPV_FORMAT_FLAG, &v);
            break;
        }
        case QMetaType::Int:
        case QMetaType::LongLong: {
            int64_t v = value.toLongLong();
            mpv_set_property(m_mpv, name.constData(), MPV_FORMAT_INT64, &v);
            break;
        }
        case QMetaType::Double:
        case QMetaType::Float: {
            double v = value.toDouble();
            mpv_set_property(m_mpv, name.constData(), MPV_FORMAT_DOUBLE, &v);
            break;
        }
        default: {
            const QByteArray s = value.toString().toUtf8();
            const char* p = s.constData();
            mpv_set_property(m_mpv, name.constData(), MPV_FORMAT_STRING, &p);
            break;
        }
    }
}

QVariant MpvPlayer::getProperty(const QByteArray& name) const {
    if (!m_mpv) return {};
    mpv_node node;
    if (mpv_get_property(const_cast<mpv_handle*>(m_mpv),
                         name.constData(), MPV_FORMAT_NODE, &node) < 0) {
        return {};
    }
    QVariant v = nodeToVariant(node);
    mpv_free_node_contents(&node);
    return v;
}

void MpvPlayer::observeProperty(const QByteArray& name) {
    if (m_mpv) mpv_observe_property(m_mpv, 0, name.constData(), MPV_FORMAT_NODE);
}

void MpvPlayer::sendCommand(const QStringList& args) {
    if (!m_mpv) return;

    std::vector<QByteArray>  storage;
    std::vector<const char*> ptrs;
    storage.reserve(args.size());
    ptrs.reserve(args.size() + 1);
    for (const QString& a : args) {
        storage.emplace_back(a.toUtf8());
        ptrs.push_back(storage.back().constData());
    }
    ptrs.push_back(nullptr);
    mpv_command_async(m_mpv, 0, ptrs.data());
}

} // namespace promp
