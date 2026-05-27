#include "TimelineStrip.h"

#include "ThumbnailWorker.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QToolTip>
#include <QtGlobal>

namespace promp {

namespace {
constexpr int kCellWidth = 138;   // 16:9 of kCellHeight (78)
constexpr int kPadLeft   = 10;    // keep thumbnails / text off the left edge
constexpr int kPadRight  = 2;
constexpr int kPadY      = 2;
}

TimelineStrip::TimelineStrip(QWidget* parent) : QWidget(parent) {
    setObjectName("TimelineStrip");
    setFixedHeight(kCellHeight + 2 * kPadY);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(12, 12, 14));
    setPalette(pal);
    setCursor(Qt::PointingHandCursor);
}

void TimelineStrip::setWorker(ThumbnailWorker* w) {
    m_worker = w;
}

void TimelineStrip::setDuration(double seconds) {
    m_duration = qMax(0.0, seconds);
    recomputeQueue();
}

void TimelineStrip::setPosition(double seconds) {
    if (qAbs(seconds - m_position) < 0.10) return;   // throttle repaint
    m_position = qMax(0.0, seconds);
    update();
}

void TimelineStrip::setSource(const QString& path) {
    if (path == m_source) return;
    m_source = path;
    m_cellImages.clear();
    recomputeQueue();
}

void TimelineStrip::onThumbnailReady(const QImage& img, double seconds) {
    if (img.isNull() || m_cellSecs.isEmpty() || m_duration <= 0) return;
    // Find the cell whose canonical timestamp is closest to `seconds`.
    int best  = -1;
    double bd = 1e9;
    for (int i = 0; i < m_cellSecs.size(); ++i) {
        const double d = qAbs(m_cellSecs[i] - seconds);
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0) return;
    if (bd > 5.0) return;   // ignore unrelated bursts
    m_cellImages[best] = img;
    update();
}

// ---------- geometry --------------------------------------------------------

int TimelineStrip::cellWidthPx() const { return kCellWidth; }

int TimelineStrip::cellCount() const {
    const int w = qMax(0, width() - kPadLeft - kPadRight);
    return qMax(1, w / kCellWidth);
}

double TimelineStrip::cellSeconds(int idx) const {
    const int n = cellCount();
    if (n <= 0) return 0;
    // Cell i covers [i/n, (i+1)/n] of duration; sample the midpoint.
    return (idx + 0.5) / double(n) * m_duration;
}

int TimelineStrip::cellAt(int x) const {
    const int n = cellCount();
    if (n <= 0) return -1;
    const int rel = qBound(0, x - kPadLeft, n * kCellWidth - 1);
    return rel / kCellWidth;
}

void TimelineStrip::recomputeQueue() {
    m_cellSecs.clear();
    if (m_duration <= 0 || !m_worker) { update(); return; }

    const int n = cellCount();
    m_cellSecs.reserve(n);
    for (int i = 0; i < n; ++i) m_cellSecs.append(cellSeconds(i));

    // Dedup images that no longer match the new cell timestamps. Cheap path:
    // just discard old cached images and let the worker repopulate.
    m_cellImages.clear();
    m_worker->requestThumbnailQueue(m_cellSecs);
    update();
}

// ---------- events ----------------------------------------------------------

void TimelineStrip::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    recomputeQueue();
}

void TimelineStrip::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || m_duration <= 0) return;
    // Free seek to the exact horizontal position (sub-cell resolution).
    const int x   = qBound(kPadLeft, int(e->position().x()), width() - kPadRight);
    const double t = double(x - kPadLeft) / double(qMax(1, width() - kPadLeft - kPadRight)) * m_duration;
    emit seekRequested(t);
}

void TimelineStrip::mouseMoveEvent(QMouseEvent* e) {
    const int idx = cellAt(int(e->position().x()));
    if (idx != m_hoverIdx) {
        m_hoverIdx = idx;
        update();
    }
    // Tooltip shows the seek target time at the cursor's x.
    if (m_duration > 0) {
        const int x   = qBound(kPadLeft, int(e->position().x()), width() - kPadRight);
        const double t = double(x - kPadLeft) / double(qMax(1, width() - kPadLeft - kPadRight)) * m_duration;
        const qint64 totalMs = qint64(t * 1000.0 + 0.5);
        const qint64 ms = totalMs % 1000;
        const qint64 s  = totalMs / 1000;
        const QString label = QString::asprintf("%lld:%02lld:%02lld.%03lld",
                                                s / 3600, (s / 60) % 60, s % 60, ms);
        QToolTip::showText(e->globalPosition().toPoint() + QPoint(16, 14), label, this);
    }
}

void TimelineStrip::leaveEvent(QEvent* e) {
    m_hoverIdx = -1;
    update();
    QWidget::leaveEvent(e);
}

// ---------- paint -----------------------------------------------------------

void TimelineStrip::paintEvent(QPaintEvent* /*e*/) {
    QPainter p(this);
    p.fillRect(rect(), QColor(12, 12, 14));

    const int n = cellCount();
    if (n <= 0 || m_duration <= 0) return;

    const int y0 = kPadY;
    const int cw = kCellWidth;

    for (int i = 0; i < n; ++i) {
        const int x0 = kPadLeft + i * cw;
        const QRect cell(x0, y0, cw - 1, kCellHeight);

        auto it = m_cellImages.constFind(i);
        if (it != m_cellImages.constEnd() && !it->isNull()) {
            // Aspect-fit the cached frame into the cell.
            QImage scaled = it->scaled(cell.size(), Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);
            const int dx = (cell.width()  - scaled.width())  / 2;
            const int dy = (cell.height() - scaled.height()) / 2;
            p.fillRect(cell, QColor(20, 20, 22));
            p.drawImage(cell.topLeft() + QPoint(dx, dy), scaled);
        } else {
            p.fillRect(cell, QColor(28, 28, 32));
            p.setPen(QColor(60, 60, 64));
            constexpr int kTextInset = 8;
            p.drawText(cell.adjusted(kTextInset, 0, -2, 0),
                       Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("…"));
        }

        if (i == m_hoverIdx) {
            p.setPen(QPen(QColor(120, 180, 255, 200), 2));
            p.drawRect(cell.adjusted(0, 0, -1, -1));
        }
    }

    // Current playback position — a thin vertical bar across the strip.
    const double frac = qBound(0.0, m_position / m_duration, 1.0);
    const int    cursorX = kPadLeft + int(frac * (n * cw - 1));
    p.fillRect(QRect(cursorX - 1, 0, 2, height()), QColor(255, 80, 80));
}

} // namespace promp
