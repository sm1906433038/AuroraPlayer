// VideoWidget.h
// QOpenGLWidget that hosts the libmpv render context.
//
// Why QOpenGLWidget (not QWidget + native window embedding):
//   * Modern, GPU-accelerated, plays nicely inside layouts.
//   * Supports HDR-aware blits when we want to compose UI on top.
//   * libmpv `render` API fully supports OpenGL FBO targets — mpv simply
//     draws into the FBO we expose each frame.

#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFramebufferObject>
#include <QPointer>
#include <QtGlobal>
#include <QMouseEvent>
#include <QPointF>
#include <QResizeEvent>
#include <QSize>

#include <memory>

#include "vr/VrRenderer.h"

namespace promp {

class MpvPlayer;
class VrController;

class VideoWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget() override;

    void attachPlayer(MpvPlayer* player);
    void attachVrController(VrController* vr);

    /// 仅在确认 OpenGL 上下文已被系统丢弃时调用（例如部分驱动在特殊操作后）；
    /// 不要在普通全屏切换时调用，否则会触发 libmpv 纹理 / INVALID_ENUM 与黑屏。
    void rebuildGlResources();

signals:
    void doubleClicked();
    void rightClicked(const QPoint& globalPos);
    void wheelScrolled(int deltaY);
    void videoClicked();   ///< single left click — host toggles play/pause

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private slots:
    void onMpvRedraw();

private:
    static void* getProcAddressStatic(void* ctx, const char* name);

    QPointer<MpvPlayer>    m_player;
    QPointer<VrController> m_vr;

    VrRenderer m_vrRenderer;
    QSize      m_videoSize;

    bool     m_vrDragging = false;
    QPointF  m_vrLastPos;

    QPointF  m_leftPressPos;
    qint64   m_lastLeftReleaseMs          = 0;
    bool     m_suppressNextLeftReleaseTimer = false;
    /// Set when the last left release emitted videoClicked(); cleared after
    /// a double-click undo or when a new mouse gesture starts.
    bool     m_pendingClickToggleUndo     = false;

    /// Non-VR: mpv renders into this RGBA8 FBO, then we blit to the widget. This
    /// avoids feeding the default framebuffer's format (HDR/sRGB) to libmpv,
    /// which has triggered GL INVALID_ENUM on Windows fullscreen for some users.
    std::unique_ptr<QOpenGLFramebufferObject> m_flatMpvFbo;
    int                                         m_flatFboW = 0;
    int                                         m_flatFboH = 0;

    /// Push the current QScreen refresh-rate into mpv (`display-fps-override`).
    /// vo=libmpv can't auto-detect refresh rate because mpv doesn't own the
    /// window — without this, `video-sync=display-resample` and
    /// `interpolation` have no target to resample to.
    void pushDisplayFpsToMpv();
    double m_lastPushedFps = 0.0;

    /// 根据当前是否在 VR 模式 + 视频分辨率，决定是否让 mpv 进入 VR 高负载
    /// 补偿（关闭 ewa / deband / hdr-compute-peak 等）。仅对 ≥ 6K 的源
    /// 启用，4K 之内的 VR 仍然走用户选定的画质预设。
    void updateVrLoadMode();
    bool m_vrLoadModeOn = false;
};

} // namespace promp
