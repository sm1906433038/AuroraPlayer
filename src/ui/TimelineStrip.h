// TimelineStrip.h
//
// A horizontal strip of evenly-spaced video thumbnails covering the whole
// duration of the current media. Click any cell to seek to that position.
//
// The strip pulls its thumbnails from the existing ThumbnailWorker via its
// new bulk-FIFO queue API. The host wires this up by:
//   * passing the worker once at construction (or via setWorker())
//   * forwarding the player's `videoSizeChanged`, `durationChanged`, and
//     `positionChanged` signals
//   * connecting the worker's `thumbnailReady` to onThumbnailReady()
//
// Sizing: the strip auto-fills its width with as many 16:9 cells as fit at
// the configured fixed height (default 78 px).

#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QSize>
#include <QWidget>

namespace promp {

class ThumbnailWorker;

class TimelineStrip : public QWidget {
    Q_OBJECT
public:
    explicit TimelineStrip(QWidget* parent = nullptr);

    void setWorker(ThumbnailWorker* w);

public slots:
    void setDuration(double seconds);
    void setPosition(double seconds);
    void setSource(const QString& path);          ///< triggers a fresh requeue.
    void onThumbnailReady(const QImage& img, double seconds);

signals:
    void seekRequested(double seconds);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    int    cellWidthPx() const;
    int    cellCount()   const;
    double cellSeconds(int idx) const;
    int    cellAt(int x) const;
    void   recomputeQueue();

    QPointer<ThumbnailWorker> m_worker;

    double                m_duration   = 0.0;
    double                m_position   = 0.0;
    QString               m_source;
    QList<double>         m_cellSecs;   // canonical timestamps for current layout
    QHash<int, QImage>    m_cellImages; // index → image

    int                   m_hoverIdx   = -1;

    static constexpr int kCellHeight = 78;        // 16:9 → ~138 px wide
};

} // namespace promp
