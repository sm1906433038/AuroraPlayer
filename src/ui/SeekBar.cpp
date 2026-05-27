#include "SeekBar.h"

#include <QApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QScreen>
#include <QStyleOptionSlider>
#include <QtMath>

namespace promp {

namespace {
QString formatTime(double seconds) {
    if (seconds < 0 || !qIsFinite(seconds)) seconds = 0;
    // Quantise to milliseconds first so the displayed digits never disagree
    // with each other due to floating-point rounding (e.g. ss=59 ms=1000).
    const qint64 totalMs = qint64(seconds * 1000.0 + 0.5);
    const qint64 ms = totalMs % 1000;
    const qint64 s  = totalMs / 1000;
    const qint64 h  = s / 3600;
    const qint64 m  = (s / 60) % 60;
    const qint64 ss = s % 60;
    return QString::asprintf("%lld:%02lld:%02lld.%03lld", h, m, ss, ms);
}
} // namespace

SeekBar::SeekBar(QWidget* parent) : QSlider(Qt::Horizontal, parent) {
    setRange(0, 100000);          // 0..1 with 1e-5 resolution
    setSingleStep(100);
    setPageStep(5000);
    setTracking(true);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);

    // Floating time tooltip. We use Qt::Tool (not Qt::ToolTip): a Tool window
    // stays visible across mouse-capture transitions on Windows, whereas a
    // ToolTip is auto-hidden by the OS when the parent grabs the mouse — which
    // is exactly what happens during a seek-bar drag, killing the popup.
    m_timeTip = new QLabel(nullptr,
                           Qt::Tool
                           | Qt::FramelessWindowHint
                           | Qt::WindowDoesNotAcceptFocus
                           | Qt::WindowStaysOnTopHint);
    m_timeTip->setAttribute(Qt::WA_ShowWithoutActivating);
    m_timeTip->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_timeTip->setAlignment(Qt::AlignCenter);
    m_timeTip->setStyleSheet(
        "QLabel { background:#101010; color:#e6e6e6; border:1px solid #303030;"
        "         padding:4px 10px; font:13px 'Segoe UI'; }");
    m_timeTip->hide();

    // Trailing-edge flush for the seek throttle: when the user stops moving
    // briefly, fire one last seekRequested for the most recent position so
    // mpv lands exactly where the cursor is rather than ~150 ms behind.
    m_seekDeferred = new QTimer(this);
    m_seekDeferred->setSingleShot(true);
    connect(m_seekDeferred, &QTimer::timeout, this, [this]() {
        if (m_pendingSeekSec >= 0 && m_duration > 0) {
            emit seekRequested(m_pendingSeekSec, /*exact*/ false);
            m_pendingSeekSec = -1.0;
            m_seekTimer.restart();
        }
    });
}

void SeekBar::setDuration(double seconds) {
    m_duration = qMax(0.0, seconds);
}

void SeekBar::setPosition(double seconds) {
    if (m_dragging || m_duration <= 0) return;
    const int v = qBound(0,
                         static_cast<int>(seconds / m_duration * maximum()),
                         maximum());
    QSignalBlocker blk(this);
    setValue(v);
}

// ----------------------------------------------------------------------------
// Mapping between cursor pixel x, slider value, and the rendered thumb-centre.
//
// QSlider's thumb has a non-zero width (kept in `SC_SliderHandle`), so the
// thumb's CENTRE can only travel along [groove.left() + hw/2,
// groove.right() - hw/2 + 1). A naive (x - groove.left()) / groove.width()
// makes the previewed timestamp drift relative to where the thumb actually
// renders — most visible near the seek-bar's edges.
// ----------------------------------------------------------------------------

double SeekBar::secondsAtX(int x) const {
    if (m_duration <= 0) return 0.0;

    QStyleOptionSlider opt;
    initStyleOption(&opt);
    const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt,
                                                 QStyle::SC_SliderGroove, this);
    const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt,
                                                 QStyle::SC_SliderHandle, this);

    const int hw   = qMax(1, handle.width());
    const int span = qMax(1, groove.width() - hw);

    const int pos = qBound(0, x - groove.left() - hw / 2, span);

    const int v = QStyle::sliderValueFromPosition(minimum(), maximum(),
                                                  pos, span,
                                                  opt.upsideDown);
    return double(v) / double(qMax(1, maximum())) * m_duration;
}

int SeekBar::thumbCentreXForSeconds(double seconds) const {
    if (m_duration <= 0) return width() / 2;

    QStyleOptionSlider opt;
    initStyleOption(&opt);
    const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt,
                                                 QStyle::SC_SliderGroove, this);
    const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt,
                                                 QStyle::SC_SliderHandle, this);

    const int hw   = qMax(1, handle.width());
    const int span = qMax(1, groove.width() - hw);
    const int v    = int(qBound(0.0, seconds, m_duration) / m_duration * maximum());

    const int pos = QStyle::sliderPositionFromValue(minimum(), maximum(),
                                                    v, span, opt.upsideDown);
    return groove.left() + pos + hw / 2;
}

void SeekBar::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && m_duration > 0) {
        m_dragging = true;
        m_scrubSignaled = false;
        // Explicit mouse capture: guarantees we keep getting mouseMoveEvent
        // even when the cursor leaves our widget rectangle mid-drag.
        grabMouse();

        const double sec = secondsAtX(int(e->position().x()));
        // Snap the slider's own thumb to the cursor immediately. Without this
        // the visual thumb only updates after mpv re-emits positionChanged
        // (which we explicitly ignore during a drag — by design).
        {
            QSignalBlocker blk(this);
            setValue(int(sec / m_duration * maximum()));
        }
        // Do NOT seek the player on press alone. A simple "click" would
        // otherwise become *two* seeks (press: keyframes + release: exact),
        // which visibly stutters. Actual seeks happen on mouse move (drag
        // throttling) + mouse release (final exact).
        showTimeTip(sec);
        e->accept();
        return;
    }
    QSlider::mousePressEvent(e);
}

void SeekBar::mouseMoveEvent(QMouseEvent* e) {
    if (m_duration <= 0) {
        QSlider::mouseMoveEvent(e);
        return;
    }
    const double sec = secondsAtX(int(e->position().x()));

    if (m_dragging) {
        if (!m_scrubSignaled) {
            m_scrubSignaled = true;
            emit seekScrubBegan();
        }
        // Keep the visible thumb glued to the cursor at full 60 Hz.
        QSignalBlocker blk(this);
        setValue(int(sec / m_duration * maximum()));

        // Throttle real mpv seeks so heavy 4K HEVC / VP9 / AV1 content can
        // actually render a frame between commands. Without this, every
        // mouseMoveEvent queues a `seek absolute+keyframes` and mpv's worker
        // never gets to display a frame — the picture appears "frozen".
        const int now = m_seekTimer.isValid() ? int(m_seekTimer.elapsed())
                                              : kSeekIntervalMs + 1;
        if (now >= kSeekIntervalMs) {
            emit seekRequested(sec, /*exact*/ false);
            m_seekTimer.restart();
            m_pendingSeekSec = -1.0;
            if (m_seekDeferred) m_seekDeferred->stop();
        } else {
            // Remember the latest target; flush after the throttle window.
            m_pendingSeekSec = sec;
            if (m_seekDeferred && !m_seekDeferred->isActive())
                m_seekDeferred->start(kSeekIntervalMs - now);
        }
    }

    showTimeTip(sec);

    // Do NOT forward to QSlider when dragging — we already updated value()
    // ourselves and QSlider's own handler would fight us over the position.
    if (!m_dragging) QSlider::mouseMoveEvent(e);
}

void SeekBar::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && m_dragging) {
        const bool hadScrub = m_scrubSignaled;
        m_scrubSignaled = false;
        m_dragging = false;
        releaseMouse();
        const double sec = secondsAtX(int(e->position().x()));
        {
            QSignalBlocker blk(this);
            setValue(int(sec / m_duration * maximum()));
        }
        // Cancel any pending throttle flush — we're issuing the final exact
        // seek right now.
        if (m_seekDeferred) m_seekDeferred->stop();
        m_pendingSeekSec = -1.0;
        emit seekRequested(sec, /*exact*/ true);
        if (hadScrub)
            emit seekScrubEnded();
        // Cursor likely still inside the bar — keep the popup for hover preview
        // until leaveEvent or the player releases focus.
    }
    QSlider::mouseReleaseEvent(e);
}

void SeekBar::leaveEvent(QEvent* e) {
    // Don't drop the popup mid-drag — the cursor may slip above or below the
    // seek bar's rectangle even while the user is still actively dragging.
    if (!m_dragging) hideTimeTip();
    QSlider::leaveEvent(e);
}

void SeekBar::paintEvent(QPaintEvent* e) {
    QSlider::paintEvent(e);
}

void SeekBar::showTimeTip(double seconds) {
    if (m_duration <= 0 || !m_timeTip) return;

    m_timeTip->setText(formatTime(seconds));
    m_timeTip->adjustSize();

    const int popupW = m_timeTip->width();
    const int popupH = m_timeTip->height();

    // Anchor on the THUMB centre, not the cursor — so the popup stays
    // visually locked onto the slider thumb regardless of any sub-pixel
    // mismatch between cursor x and Qt's thumb rendering.
    const int  thumbX = thumbCentreXForSeconds(seconds);
    const QPoint anchor = mapToGlobal(QPoint(thumbX - popupW / 2,
                                             -popupH - 8));

    // Clamp horizontally so the popup never escapes the screen edge.
    QPoint placed = anchor;
    if (auto* scr = screen()) {
        const QRect g = scr->geometry();
        placed.setX(qBound(g.left() + 4,
                           anchor.x(),
                           g.right() - popupW - 4));
        // If the seek bar is so close to the top we'd clip, slide popup below.
        if (placed.y() < g.top() + 4)
            placed.setY(mapToGlobal(QPoint(0, height() + 8)).y());
    }
    m_timeTip->move(placed);

    if (!m_timeTip->isVisible()) m_timeTip->show();
    m_timeTip->raise();
}

void SeekBar::hideTimeTip() {
    if (m_timeTip) m_timeTip->hide();
}

} // namespace promp
