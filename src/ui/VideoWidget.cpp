#include "VideoWidget.h"
#include "core/MpvPlayer.h"
#include "vr/VrController.h"

#include <mpv/render.h>
#include <mpv/render_gl.h>

#include <QApplication>
#include <QDateTime>
#include <QResizeEvent>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QScreen>
#include <QSurfaceFormat>
#include <QWindow>
#include <QtMath>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <memory>

namespace promp {

namespace {

// GL enums (avoid relying on a platform GL header in this translation unit).
constexpr unsigned kGlRgba8            = 0x8058;
constexpr unsigned kGlReadFramebuffer  = 0x8CA8;
constexpr unsigned kGlDrawFramebuffer  = 0x8CA9;
constexpr unsigned kGlFramebuffer      = 0x8D40;
constexpr unsigned kGlColorBufferBit   = 0x00004000;
constexpr unsigned kGlNearest          = 0x2600;

} // namespace

VideoWidget::VideoWidget(QWidget* parent) : QOpenGLWidget(parent) {
    // Keep GL 3.3 Core in sync with QSurfaceFormat::setDefaultFormat (main.cpp).
    // GL 4.x + mpv high-quality + HDR peak has produced INVALID_ENUM after
    // fullscreen transitions on some Windows + QOpenGLWidget setups.
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    fmt.setSwapInterval(1);  // VSYNC on — pairs with mpv's display-resample.
    fmt.setSamples(0);
    setFormat(fmt);

    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

void VideoWidget::attachVrController(VrController* vr) {
    if (m_vr) disconnect(m_vr, nullptr, this, nullptr);
    m_vr = vr;
    if (m_vr) {
        // Live update when the user drags / zooms / changes projection.
        connect(m_vr, &VrController::changed, this, [this]() {
            updateVrLoadMode();
            update();
        });
    }
    updateVrLoadMode();
}

VideoWidget::~VideoWidget() {
    if (m_player) {
        makeCurrent();
        m_flatMpvFbo.reset();
        m_vrRenderer.shutdown();
        m_player->destroyRenderContext();
        doneCurrent();
    }
}

void VideoWidget::attachPlayer(MpvPlayer* player) {
    if (m_player) {
        disconnect(m_player, nullptr, this, nullptr);
    }
    m_player = player;
    if (m_player) {
        connect(m_player, &MpvPlayer::renderUpdateRequested,
                this, &VideoWidget::onMpvRedraw, Qt::QueuedConnection);
        // Cache video size so the VR FBO can sample at native resolution.
        connect(m_player, &MpvPlayer::videoSizeChanged, this, [this](QSize sz) {
            m_videoSize = sz;
            updateVrLoadMode();
            update();
        });
    }
    updateVrLoadMode();
}

void* VideoWidget::getProcAddressStatic(void* ctx, const char* name) {
    Q_UNUSED(ctx);
    QOpenGLContext* gl = QOpenGLContext::currentContext();
    if (!gl) return nullptr;
    return reinterpret_cast<void*>(gl->getProcAddress(QByteArray(name)));
}

void VideoWidget::initializeGL() {
    if (!m_player) {
        qWarning() << "VideoWidget::initializeGL: no MpvPlayer attached";
        return;
    }
    m_flatMpvFbo.reset();
    m_flatFboW = m_flatFboH = 0;
    if (!m_player->createRenderContext(&VideoWidget::getProcAddressStatic, this)) {
        qWarning() << "VideoWidget: failed to create mpv render context";
    }
    if (!m_vrRenderer.initialize()) {
        qWarning() << "VideoWidget: VR renderer init failed; sphere mode unavailable";
    }
}

void VideoWidget::rebuildGlResources() {
    if (!m_player) return;
    makeCurrent();
    m_flatMpvFbo.reset();
    m_flatFboW = m_flatFboH = 0;
    m_player->destroyRenderContext();
    m_vrRenderer.shutdown();
    if (!m_player->createRenderContext(&VideoWidget::getProcAddressStatic, this)) {
        qWarning() << "VideoWidget: rebuildGlResources: mpv render context failed";
    }
    if (!m_vrRenderer.initialize()) {
        qWarning() << "VideoWidget: rebuildGlResources: VR renderer failed";
    }
    doneCurrent();
    update();
}

void VideoWidget::paintGL() {
    if (!m_player || !m_player->renderContext()) return;

    if (!QOpenGLContext::currentContext())
        return;

    const auto dpr       = devicePixelRatioF();
    const int  viewportW = std::max(1, int(width()  * dpr));
    const int  viewportH = std::max(1, int(height() * dpr));

    const bool useVr =
        m_vr && m_vr->isEnabled() && m_vrRenderer.ready();

    if (useVr) {
        // ---- pass 1: mpv → intermediate FBO ---------------------------------
        //
        // FBO 尺寸策略：
        //
        //   - 必须保持视频源 *纵横比*（否则 8192×4096 被独立 clamp 成
        //     4096×4096 后，equirect 球面采样会左右压扁、画面变形）。
        //
        //   - 大小并不需要等于源分辨率。球面采样时，视野里同时只能看到
        //     全景的一小块；中间 FBO 比 viewport 大太多就是纯浪费 GPU
        //     带宽 + 让 mpv 的 deband / ewa_lanczossharp / hdr-compute-peak
        //     在 33 MP（8K）上空转。
        //
        //     折中：FBO 长边 = max(viewport 长边 × 2, 1024)，封顶 8192，
        //     且不超过视频原生长边（永远不做 *上* 采样）。系数 2 给
        //     旋转 / 缩放视角留出过采样余量；用户拉到 4K 显示器全屏
        //     时 FBO 仍能升到 ~7680，足够锐利。
        //
        //   - 现代桌面 GPU GL_MAX_TEXTURE_SIZE 通常 ≥ 16384；8192×4096
        //     RGBA8 仅占 128 MiB 显存。
        constexpr int kMaxDim       = 8192;
        constexpr int kMinDim       = 1024;
        constexpr int kOversample   = 2;

        int srcW = viewportW;
        int srcH = viewportH;
        if (m_videoSize.isValid()) {
            const int srcLong = std::max(m_videoSize.width(),
                                         m_videoSize.height());
            const int vpLong  = std::max(viewportW, viewportH);

            int targetLong = std::clamp(vpLong * kOversample, kMinDim, kMaxDim);
            if (targetLong > srcLong) targetLong = srcLong;  // 不上采样

            const double s = double(targetLong) / double(srcLong);
            srcW = std::max(1, int(std::lround(m_videoSize.width()  * s)));
            srcH = std::max(1, int(std::lround(m_videoSize.height() * s)));
        }
        m_vrRenderer.resizeIntermediate(srcW, srcH);

        mpv_opengl_fbo fbo{};
        fbo.fbo             = int(m_vrRenderer.fboId());
        fbo.w               = m_vrRenderer.fboW();
        fbo.h               = m_vrRenderer.fboH();
        fbo.internal_format = 0;
        int flip_y = 1;
        mpv_render_param params[]{
            { MPV_RENDER_PARAM_OPENGL_FBO, &fbo },
            { MPV_RENDER_PARAM_FLIP_Y,     &flip_y },
            { MPV_RENDER_PARAM_INVALID,    nullptr },
        };
        mpv_render_context_render(m_player->renderContext(), params);

        // ---- pass 2: sphere projection → backbuffer.
        QOpenGLFunctions* gl = QOpenGLContext::currentContext()->functions();
        gl->glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        gl->glViewport(0, 0, viewportW, viewportH);
        gl->glClearColor(0.f, 0.f, 0.f, 1.f);
        gl->glClear(GL_COLOR_BUFFER_BIT);

        m_vrRenderer.draw(viewportW, viewportH,
                          m_vr->projection(),
                          m_vr->stereo(),
                          m_vr->yaw(), m_vr->pitch(), m_vr->fov());
        mpv_render_context_report_swap(m_player->renderContext());
        pushDisplayFpsToMpv();
        return;
    }

    // ---- Non-VR: mpv → explicit RGBA8 FBO → blit to the widget framebuffer.
    // Rendering directly into QOpenGLWidget's default framebuffer can break
    // libmpv's texture setup on Windows fullscreen (HDR / sRGB backbuffers),
    // producing GL INVALID_ENUM and a black image.
    QOpenGLExtraFunctions* ef = QOpenGLContext::currentContext()->extraFunctions();
    if (!ef) {
        qWarning() << "VideoWidget::paintGL: OpenGL extra functions unavailable";
        return;
    }

    if (!m_flatMpvFbo || m_flatFboW != viewportW || m_flatFboH != viewportH) {
        m_flatMpvFbo.reset();
        QOpenGLFramebufferObjectFormat fboFmt;
        fboFmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);
        fboFmt.setSamples(0);
        fboFmt.setInternalTextureFormat(static_cast<GLenum>(kGlRgba8));
        m_flatMpvFbo = std::make_unique<QOpenGLFramebufferObject>(viewportW, viewportH, fboFmt);
        m_flatFboW = viewportW;
        m_flatFboH = viewportH;
    }

    mpv_opengl_fbo fbo{};
    fbo.fbo             = int(m_flatMpvFbo->handle());
    fbo.w               = viewportW;
    fbo.h               = viewportH;
    fbo.internal_format = int(kGlRgba8);
    int flip_y = 1;
    mpv_render_param params[]{
        { MPV_RENDER_PARAM_OPENGL_FBO, &fbo },
        { MPV_RENDER_PARAM_FLIP_Y,     &flip_y },
        { MPV_RENDER_PARAM_INVALID,    nullptr },
    };
    mpv_render_context_render(m_player->renderContext(), params);

    const GLuint readName = m_flatMpvFbo->handle();
    const GLuint drawName = GLuint(defaultFramebufferObject());
    ef->glBindFramebuffer(GLenum(kGlReadFramebuffer), readName);
    ef->glBindFramebuffer(GLenum(kGlDrawFramebuffer), drawName);
    ef->glBlitFramebuffer(0, 0, viewportW, viewportH,
                          0, 0, viewportW, viewportH,
                          GLbitfield(kGlColorBufferBit), GLenum(kGlNearest));
    ef->glBindFramebuffer(GLenum(kGlFramebuffer), drawName);

    // Tell mpv that a frame was just presented + report current refresh
    // rate. Both are required for video-sync=display-resample and
    // interpolation to function — vo=libmpv can't infer either by itself.
    mpv_render_context_report_swap(m_player->renderContext());
    pushDisplayFpsToMpv();
}

void VideoWidget::resizeGL(int /*w*/, int /*h*/) {
    // mpv reads target size each frame from the FBO param. No-op.
}

void VideoWidget::updateVrLoadMode() {
    if (!m_player) return;
    // 阈值：源长边 ≥ 6144 时进入 VR 高负载补偿。
    // 4096×2048（4K VR）= 4096 < 6144，保持用户选择的画质预设；
    // 8192×4096（8K VR）= 8192 ≥ 6144，自动降档以维持帧率。
    constexpr int kHeavyThreshold = 6144;
    const bool vrActive = m_vr && m_vr->isEnabled();
    const bool heavy    = m_videoSize.isValid()
                       && std::max(m_videoSize.width(), m_videoSize.height())
                          >= kHeavyThreshold;
    const bool want = vrActive && heavy;
    if (want == m_vrLoadModeOn) return;
    m_vrLoadModeOn = want;
    m_player->setVrLoadMode(want);
}

void VideoWidget::pushDisplayFpsToMpv() {
    if (!m_player) return;
    QScreen* scr = nullptr;
    if (auto* w = window(); w) {
        if (auto* wh = w->windowHandle(); wh) scr = wh->screen();
    }
    if (!scr) scr = QGuiApplication::primaryScreen();
    if (!scr) return;
    const double hz = scr->refreshRate();
    if (hz <= 1.0) return;
    if (std::fabs(hz - m_lastPushedFps) < 0.1) return;   // unchanged
    m_lastPushedFps = hz;
    // display-fps-override is the modern read/write property; the legacy
    // display-fps name still exists on older mpv but is read-only there.
    m_player->setProperty("display-fps-override", hz);
}

void VideoWidget::onMpvRedraw() {
    update();
}

// ---------- input ------------------------------------------------------------

void VideoWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    m_suppressNextLeftReleaseTimer = false;
    if (e->button() == Qt::LeftButton) {
        if (m_pendingClickToggleUndo) {
            emit videoClicked();
            m_pendingClickToggleUndo = false;
        }
        emit doubleClicked();
    }
    QOpenGLWidget::mouseDoubleClickEvent(e);
}

void VideoWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_leftPressPos = e->position();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_lastLeftReleaseMs > 0
            && (now - m_lastLeftReleaseMs) > QApplication::doubleClickInterval())
            m_pendingClickToggleUndo = false;
        if (m_lastLeftReleaseMs > 0
            && (now - m_lastLeftReleaseMs) <= QApplication::doubleClickInterval())
            m_suppressNextLeftReleaseTimer = true;
        else
            m_suppressNextLeftReleaseTimer = false;
    }
    if (e->button() == Qt::RightButton) {
        e->accept();
        emit rightClicked(e->globalPosition().toPoint());
    } else if (e->button() == Qt::LeftButton && m_vr && m_vr->isEnabled()) {
        m_vrDragging = true;
        m_vrLastPos  = e->position();
        setCursor(Qt::ClosedHandCursor);
    }
    QOpenGLWidget::mousePressEvent(e);
}

void VideoWidget::mouseMoveEvent(QMouseEvent* e) {
    if (m_vrDragging && m_vr) {
        const QPointF d = e->position() - m_vrLastPos;
        m_vrLastPos = e->position();
        m_vr->dragRotate(d.x(), d.y());
    }
    QOpenGLWidget::mouseMoveEvent(e);
}

void VideoWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_lastLeftReleaseMs = QDateTime::currentMSecsSinceEpoch();

        const double dist = (e->position() - m_leftPressPos).manhattanLength();
        if (m_vr && m_vr->isEnabled()) {
            if (m_vrDragging) {
                m_vrDragging = false;
                unsetCursor();
            }
            if (dist < 15.0) {
                if (m_suppressNextLeftReleaseTimer) {
                    m_suppressNextLeftReleaseTimer = false;
                } else {
                    emit videoClicked();
                    m_pendingClickToggleUndo = true;
                }
            }
        } else if (dist < 15.0) {
            if (m_suppressNextLeftReleaseTimer) {
                m_suppressNextLeftReleaseTimer = false;
            } else {
                emit videoClicked();
                m_pendingClickToggleUndo = true;
            }
        }
    }
    QOpenGLWidget::mouseReleaseEvent(e);
}

void VideoWidget::wheelEvent(QWheelEvent* e) {
    if (m_vr && m_vr->isEnabled()) {
        m_vr->zoom(e->angleDelta().y());
        e->accept();
        return;
    }
    emit wheelScrolled(e->angleDelta().y());
    QOpenGLWidget::wheelEvent(e);
}

void VideoWidget::resizeEvent(QResizeEvent* e) {
    QOpenGLWidget::resizeEvent(e);
    // After top-level resize / fullscreen, schedule an immediate paint so mpv
    // fills the framebuffer before the window composes a black cleared frame.
    update();
}

} // namespace promp
