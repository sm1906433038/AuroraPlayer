// MpvPlayer.h
// Thin, Qt-friendly wrapper around libmpv.
//
// Responsibilities:
//   * Owns the mpv_handle and its render context.
//   * Translates mpv asynchronous events / property changes into Qt signals.
//   * Exposes typed setters/getters for common properties.
//
// Threading:
//   * mpv emits its wakeup callback from an arbitrary thread. We post a queued
//     signal back to the GUI thread, where we drain mpv_wait_event() loop.
//   * mpv_render_context update callbacks ALSO arrive on an unknown thread.
//     They are forwarded to VideoWidget via a queued signal so QOpenGLWidget
//     can schedule an update on the GUI thread safely.

#pragma once

#include <QList>
#include <QObject>
#include <QSize>
#include <QString>
#include <QVariant>

struct mpv_handle;
struct mpv_render_context;

namespace promp {

/// One entry in mpv's track-list. We capture the fields most useful for UI
/// presentation and keep the raw mpv id (which is what `aid` / `sid` / `vid`
/// take). `id == 0` means "no track" / disabled.
struct TrackInfo {
    enum class Type { Video, Audio, Subtitle, Unknown };

    int     id        = 0;            ///< mpv "id" — the value to assign to aid/sid/vid.
    Type    type      = Type::Unknown;
    QString title;                    ///< user-supplied label, may be empty.
    QString lang;                     ///< ISO 639 if known, e.g. "eng".
    QString codec;                    ///< codec short name, e.g. "h264", "aac".
    bool    selected  = false;
    bool    external  = false;        ///< loaded via `--audio-file` / `--sub-file`.
    QString filename; ///< for external tracks
};

class MpvPlayer : public QObject {
    Q_OBJECT
public:
    explicit MpvPlayer(QObject* parent = nullptr);
    ~MpvPlayer() override;

    MpvPlayer(const MpvPlayer&) = delete;
    MpvPlayer& operator=(const MpvPlayer&) = delete;

    // Raw handle access for the render bridge. Treat as opaque elsewhere.
    [[nodiscard]] mpv_handle* handle() const noexcept { return m_mpv; }

    // Render context lifecycle: VideoWidget calls these with a live OpenGL ctx.
    bool createRenderContext(void* (*get_proc_address)(void* ctx, const char* name),
                             void* get_proc_address_ctx);
    void destroyRenderContext();
    [[nodiscard]] mpv_render_context* renderContext() const noexcept { return m_render; }

    // -------- Commands --------
    bool loadFile(const QString& path, bool append = false);
    void play();
    void pause();
    void togglePause();
    void stop();
    void seekAbsolute(double seconds, bool exact = false);
    void seekRelative(double seconds, bool exact = false);
    void setSpeed(double speed);
    void setVolume(double percent);   // 0..100 (or up to 130)
    void setMute(bool muted);
    void screenshot(const QString& path = {});

    // -------- Playback fine controls --------
    void frameStep();              ///< advance exactly one frame, then pause.
    void frameBackStep();          ///< the reverse — slower (mpv has to seek).
    void stepSpeed(double delta);  ///< add `delta` to current speed (clamped 0.05..10).
    void resetSpeed();             ///< speed = 1.0
    void cycleAudio(int dir = +1); ///< -1 / +1
    void cycleSubtitle(int dir = +1);
    void toggleSubtitleVisibility();
    void setSubtitleDelay(double seconds);
    void setAudioDelay(double seconds);

    // -------- Track switching --------
    /// Switch to the given mpv track id. Pass 0 to disable.
    void setAudioTrack(int id);
    void setSubtitleTrack(int id);
    void setVideoTrack(int id);
    /// Snapshot of the current track-list (parsed in onWakeUp on `track-list` change).
    [[nodiscard]] QList<TrackInfo> tracks() const { return m_tracks; }

    // -------- OSD --------
    /// Show a transient on-screen message via mpv's built-in OSD.
    void showText(const QString& msg, int durationMs = 1500);

    // -------- Quality / picture-quality preset ---------------------------
    /// Three coarse levels controlling the GPU shader / scaler / dither
    /// / debanding / HDR / interpolation chain. Standard is the safe
    /// fallback (matches the historical baseline); High is the recommended
    /// daily setting for a modern GPU; Ultra adds 24p->display-rate
    /// interpolation and is meant for high-refresh displays.
    enum class QualityPreset {
        Standard = 0,
        High     = 1,
        Ultra    = 2,
        Native   = 3,   ///< Zero post-processing: bilinear, no deband, no
                        ///< tone-mapping, no gamut-mapping, no interpolation.
                        ///< Use when you want the source pixels untouched.
    };
    void setQualityPreset(QualityPreset p);
    [[nodiscard]] QualityPreset qualityPreset() const noexcept { return m_qualityPreset; }

    /// VR 高负载补偿：球面 + 8K 场景下，第一次渲染要在 8192×4096 这种巨型
    /// 中间 FBO 上跑完 mpv 整套高质量管线（ewa_lanczossharp / deband /
    /// hdr-compute-peak / linear-downscaling …），单帧 GPU 耗时会爆。开启
    /// 此开关时，临时把这些昂贵的 shader 切到便宜档；关闭时自动恢复用户
    /// 选定的 QualityPreset。不修改 m_qualityPreset，不持久化。
    void setVrLoadMode(bool enabled);
    [[nodiscard]] bool vrLoadMode() const noexcept { return m_vrLoadMode; }

    // -------- Hardware acceleration -------------------------------------
    /// Three hwdec modes. CopyMode runs the GPU decoder but copies frames
    /// back to system memory before re-uploading to the GL renderer —
    /// trivially compatible but the round-trip kills 8K and VR. ZeroCopy
    /// keeps the decoded frame on the GPU through interop and is the
    /// performant choice on modern dGPUs. Off forces software decoding.
    enum class HwAccel {
        Off       = 0,
        CopyMode  = 1,
        ZeroCopy  = 2,
    };
    void setHwAccel(HwAccel a);
    [[nodiscard]] HwAccel hwAccel() const noexcept { return m_hwAccel; }

    // -------- Generic property bridge --------
    void setProperty(const QByteArray& name, const QVariant& value);
    [[nodiscard]] QVariant getProperty(const QByteArray& name) const;
    void observeProperty(const QByteArray& name);
    void sendCommand(const QStringList& args);

signals:
    // Fired (queued) when mpv has events pending — we drain them on GUI thread.
    void wakeUp();
    // Fired (queued) when the render context wants a new frame.
    void renderUpdateRequested();

    // High-level state changes (parsed from mpv property events)
    void fileLoaded(const QString& path);
    void endFile(int reason);
    void durationChanged(double seconds);
    void positionChanged(double seconds);
    void pausedChanged(bool paused);
    void volumeChanged(double percent);
    void muteChanged(bool muted);
    void videoSizeChanged(QSize size);
    void speedChanged(double speed);
    void mediaTitleChanged(const QString& title);
    void tracksChanged();
    void qualityPresetChanged(promp::MpvPlayer::QualityPreset p);
    void hwAccelChanged(promp::MpvPlayer::HwAccel a);
    void logMessage(const QString& level, const QString& component, const QString& text);

private slots:
    void onWakeUp();

private:
    void applyBaselineOptions();
    /// Apply the picture-quality preset via runtime properties (must be
    /// called after mpv_initialize()).
    void applyQualityPreset(QualityPreset p);
    void registerObservers();
    void refreshTrackList();

    static void wakeupCb(void* ctx) noexcept;
    static void renderUpdateCb(void* ctx) noexcept;

    mpv_handle*         m_mpv    = nullptr;
    mpv_render_context* m_render = nullptr;

    QList<TrackInfo>    m_tracks;
    QualityPreset       m_qualityPreset = QualityPreset::Standard;
    HwAccel             m_hwAccel       = HwAccel::CopyMode;
    bool                m_vrLoadMode    = false;
};

} // namespace promp
