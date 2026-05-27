// ThumbnailWorker.h
//
// Generates thumbnail frames at arbitrary timestamps using a *second*,
// headless mpv instance. We use mpv's `screenshot-raw` command which returns
// a node with the raw BGRA bytes — no temp files, no QProcess, no FFmpeg
// shipping required.
//
// Design:
//   * Lives on its own QThread.
//   * Maintains a single most-recent pending request (drops older ones)
//     so dragging the seek bar fast doesn't queue up dozens of seeks.
//   * Small LRU cache keyed by quantized (file, second).

#pragma once

#include <QObject>
#include <QThread>
#include <QImage>
#include <QHash>
#include <QList>
#include <QQueue>
#include <QString>
#include <QMutex>
#include <QWaitCondition>

struct mpv_handle;

namespace promp {

class ThumbnailWorker : public QObject {
    Q_OBJECT
public:
    explicit ThumbnailWorker(QObject* parent = nullptr);
    ~ThumbnailWorker() override;

    void start();
    void stop();

public slots:
    void setSource(const QString& path);
    /// High-priority single request (the *latest* call wins). Used by the
    /// seek-bar hover preview which only cares about the cursor's current pos.
    void requestThumbnail(double seconds);
    /// Bulk FIFO request used by the timeline strip: enqueue many positions
    /// and let the worker grind through them in order. Priority requests
    /// always preempt the queue.
    void requestThumbnailQueue(const QList<double>& seconds);
    void clearQueue();

signals:
    void thumbnailReady(const QImage& img, double seconds);

private:
    void run();                                   // worker loop
    /// Hi-quality preview seeks are frame-accurate; bulk strip seeks stay fast.
    QImage grab(double seconds, bool previewPriority); // worker thread only

    QThread       m_thread;
    mpv_handle*   m_mpv = nullptr;

    QMutex          m_mutex;
    QWaitCondition  m_cv;
    QString         m_source;
    QString         m_loadedSource;
    double          m_requested      = -1.0;       // priority request
    QQueue<double>  m_queue;                       // bulk FIFO requests
    bool            m_quit           = false;

    // LRU cache: 1-second bucket → image. Bumped to 128 so a 24-cell timeline
    // strip + active hover preview do not thrash.
    /// Key packs previewPriority into high bit + millisecond time to avoid
    /// mixing keyframe-strip thumbs with exact seek-bar thumbs.
    QHash<quint64, QImage> m_cache;
    QList<quint64>        m_cacheOrder;
    static constexpr int kCacheLimit = 128;
};

} // namespace promp
