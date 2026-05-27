// SeekBar.h
// A QSlider-derived seek bar that:
//   * Jumps to the clicked position (no drag-from-edge).
//   * Shows a small floating tooltip with the time at the cursor.
//   * Emits seekRequested(seconds, exact) for drag throttling + release.

#pragma once

#include <QElapsedTimer>
#include <QPointer>
#include <QSlider>
#include <QTimer>

class QLabel;

namespace promp {

class SeekBar : public QSlider {
    Q_OBJECT
public:
    explicit SeekBar(QWidget* parent = nullptr);

    void setDuration(double seconds);
    void setPosition(double seconds);

    [[nodiscard]] double duration() const noexcept { return m_duration; }

signals:
    void seekRequested(double seconds, bool exact);
    /// Emitted on the first mouse-move after press (true scrub), before throttled seeks.
    void seekScrubBegan();
    /// After mouse release; pair with seekScrubBegan if that was emitted.
    void seekScrubEnded();

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;

private:
    [[nodiscard]] double secondsAtX(int x) const;
    /// Pixel x (within this widget) of the slider thumb's centre for `seconds`.
    [[nodiscard]] int    thumbCentreXForSeconds(double seconds) const;
    /// Place + show the time tooltip above the thumb-centre pixel for `seconds`.
    void showTimeTip(double seconds);
    void hideTimeTip();

    double m_duration = 0.0;
    bool   m_dragging = false;

    QPointer<QLabel> m_timeTip;     // floating tooltip-style label

    /// Heavy-codec drag throttle. The slider value updates every mouse move,
    /// but `seekRequested` is emitted at up to ~100 Hz (see `kSeekIntervalMs`).
    /// Lower = more responsive video scrubbing; too low and 4K/8K may fall
    /// behind (mpv seek queue) — raise `kSeekIntervalMs` if that happens.
    QElapsedTimer    m_seekTimer;
    QTimer*          m_seekDeferred = nullptr;
    double           m_pendingSeekSec  = -1.0;
    bool             m_scrubSignaled = false;
    /// Minimum gap between `seekRequested` emissions while dragging (main player).
    static constexpr int kSeekIntervalMs = 10;
};

} // namespace promp
