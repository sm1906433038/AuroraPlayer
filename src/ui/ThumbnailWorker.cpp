#include "ThumbnailWorker.h"

#include <mpv/client.h>

#include <QDebug>
#include <QMutexLocker>
#include <QtEndian>
#include <QtMath>
#include <cmath>

namespace promp {

namespace {

/// Cache rows are kept distinct for fast keyframe strip thumbs vs frame-
/// accurate seek-bar preview thumbs — they can decode to different frames
/// for the same timestamp, and the preview must never substitute a coarse
/// keyframe screenshot when the user is asking for the exact frame.
[[nodiscard]] quint64 thumbCacheKey(double seconds, bool previewPriority) noexcept {
    const double       clamped = qBound(0.0, seconds, 86400.0 * 365.0);
    const qint64       ms      = qint64(qRound(clamped * 1000.0));
    constexpr quint64  kPri    = (quint64{1} << 62);
    return (previewPriority ? kPri : 0) | (quint64(ms) & 0x3fffffffffffffffULL);
}

/// `loadfile` is asynchronous: if we `seek` / `screenshot-raw` immediately we
/// still have the old demuxer (or nothing) and the grab returns a null image —
/// which broke the seek-bar preview whenever a new file loaded. Wait until mpv
/// signals `FILE_LOADED` (or irrecoverable failure) before generating thumbs.
bool waitAfterLoadfile(mpv_handle* mpv) {
    for (int n = 0; n < 800; ++n) { // up to ~80 s @ 100 ms (slow NAS / 8K open)
        mpv_event* ev = mpv_wait_event(mpv, 0.1);
        if (!ev) continue;
        if (ev->event_id == MPV_EVENT_FILE_LOADED) return true;
        if (ev->event_id == MPV_EVENT_END_FILE) return false;
        if (ev->event_id == MPV_EVENT_SHUTDOWN) return false;
    }
    return false;
}

void waitSeekStable(mpv_handle* mpv, double targetSec, bool tightAccuracy) {
    const int   maxIter  = tightAccuracy ? 500 : 150;
    const double eps     = tightAccuracy ? 0.45 : 1.5;
    for (int i = 0; i < maxIter; ++i) {
        double pos = 0;
        if (mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos) >= 0
            && std::isfinite(pos) && qAbs(pos - targetSec) < eps)
            break;
        mpv_event* ev = mpv_wait_event(mpv, tightAccuracy ? 0.04 : 0.03);
        if (ev && ev->event_id == MPV_EVENT_PLAYBACK_RESTART)
            continue;
        if (i > 12 && ev && ev->event_id == MPV_EVENT_NONE && !tightAccuracy)
            break; // keyframe seek: don't spin forever
    }
}

} // namespace

ThumbnailWorker::ThumbnailWorker(QObject* /*parent*/) : QObject(nullptr) {
    // IMPORTANT: must be parent-less so moveToThread() succeeds. Qt forbids
    // moving an object that has a parent to a different thread. Lifetime is
    // managed manually by the caller (MainWindow deletes us in its dtor).
    moveToThread(&m_thread);
    connect(&m_thread, &QThread::started, this, [this]() { run(); });
}

ThumbnailWorker::~ThumbnailWorker() {
    stop();
}

void ThumbnailWorker::start() {
    if (!m_thread.isRunning()) m_thread.start();
}

void ThumbnailWorker::stop() {
    {
        QMutexLocker lk(&m_mutex);
        m_quit = true;
        m_cv.wakeAll();
    }
    if (m_thread.isRunning()) {
        m_thread.quit();
        m_thread.wait();
    }
}

void ThumbnailWorker::setSource(const QString& path) {
    QMutexLocker lk(&m_mutex);
    if (path != m_source) {
        m_source       = path;
        m_loadedSource.clear();   // forces a reload on the worker thread
        m_cache.clear();
        m_cacheOrder.clear();
        m_queue.clear();
        m_requested = -1.0;
    }
    m_cv.wakeAll();
}

void ThumbnailWorker::requestThumbnail(double seconds) {
    QMutexLocker lk(&m_mutex);
    m_requested = seconds;
    m_cv.wakeAll();
}

void ThumbnailWorker::requestThumbnailQueue(const QList<double>& seconds) {
    QMutexLocker lk(&m_mutex);
    m_queue.clear();
    for (double s : seconds) m_queue.enqueue(s);
    m_cv.wakeAll();
}

void ThumbnailWorker::clearQueue() {
    QMutexLocker lk(&m_mutex);
    m_queue.clear();
}

// -----------------------------------------------------------------------------

void ThumbnailWorker::run() {
    m_mpv = mpv_create();
    if (!m_mpv) {
        qWarning() << "ThumbnailWorker: mpv_create failed";
        return;
    }

    auto opt = [this](const char* k, const char* v) {
        mpv_set_option_string(m_mpv, k, v);
    };
    opt("vo",                   "null");
    opt("ao",                   "null");
    opt("audio",                "no");
    // Must NOT use auto-safe here: on Windows it probes Vulkan interop and can
    // deadlock or spin against a concurrently-initialising main libmpv instance.
    opt("gpu-hwdec-interop",    "no");
    opt("hwdec",                "d3d11va-copy,nvdec-copy,no");
    opt("hr-seek",              "yes");
    opt("hr-seek-framedrop",    "yes");
    opt("cache",                "yes");
    opt("demuxer-max-bytes",    "64MiB");
    opt("untimed",              "yes");
    opt("force-window",         "no");
    opt("idle",                 "yes");
    opt("video-sync",           "audio");

    if (mpv_initialize(m_mpv) < 0) {
        qWarning() << "ThumbnailWorker: mpv_initialize failed";
        mpv_destroy(m_mpv); m_mpv = nullptr;
        return;
    }

    for (;;) {
        QString sourceToLoad;
        double  seconds       = -1.0;
        bool    fromPriority  = false;
        {
            QMutexLocker lk(&m_mutex);
            while (!m_quit
                   && m_requested < 0
                   && m_queue.isEmpty()
                   && m_source == m_loadedSource) {
                m_cv.wait(&m_mutex);
            }
            if (m_quit) break;

            if (m_source != m_loadedSource) {
                sourceToLoad   = m_source;
                m_loadedSource = m_source;
            }
            // Priority request always beats the FIFO queue.
            if (m_requested >= 0) {
                seconds       = m_requested;
                m_requested   = -1.0;
                fromPriority  = true;
            } else if (!m_queue.isEmpty()) {
                seconds       = m_queue.dequeue();
                fromPriority  = false;
            }
        }

        if (!sourceToLoad.isEmpty()) {
            const QByteArray u8 = sourceToLoad.toUtf8();
            const char* args[] = { "loadfile", u8.constData(), "replace", nullptr };
            if (mpv_command(m_mpv, args) < 0) {
                if (seconds >= 0)
                    emit thumbnailReady({}, seconds);
                continue;
            }
            if (!waitAfterLoadfile(m_mpv)) {
                if (seconds >= 0)
                    emit thumbnailReady({}, seconds);
                continue;
            }
        }

        if (seconds >= 0) {
            const quint64 key = thumbCacheKey(seconds, fromPriority);
            QImage          cached;
            {
                QMutexLocker lk(&m_mutex);
                cached = m_cache.value(key);
            }
            if (!cached.isNull()) {
                emit thumbnailReady(cached, seconds);
                continue;
            }

            const QImage img = grab(seconds, fromPriority);
            if (!img.isNull()) {
                QMutexLocker lk(&m_mutex);
                m_cache.insert(key, img);
                m_cacheOrder.append(key);
                while (m_cacheOrder.size() > kCacheLimit) {
                    m_cache.remove(m_cacheOrder.takeFirst());
                }
            }
            emit thumbnailReady(img, seconds);
        }
    }

    mpv_terminate_destroy(m_mpv);
    m_mpv = nullptr;
}

QImage ThumbnailWorker::grab(double seconds, bool previewPriority) {
    if (!m_mpv) return {};
    if (!std::isfinite(seconds)) return {};

    // Preview bar: frame-accurate seeks so the image content always matches
    // the time the user is hovering on. Slow on long-GOP / 4K content, but
    // the SeekBar drops stale frames so the user never sees a wrong picture
    // — they just see the last good one until the new one arrives.
    // Timeline strip bulk jobs: keyframe seeks (fast bulk fill is more
    // important than per-cell precision).
    const QByteArray ts   = QByteArray::number(seconds, 'f', 3);
    const char*      mode = previewPriority ? "absolute+exact" : "absolute+keyframes";
    const char* seek_args[] = { "seek", ts.constData(), mode, nullptr };
    if (mpv_command(m_mpv, seek_args) < 0) return {};

    waitSeekStable(m_mpv, seconds, previewPriority);

    mpv_node node;
    const char* shot_args[] = { "screenshot-raw", "video", nullptr };
    if (mpv_command_ret(m_mpv, shot_args, &node) < 0) return {};

    QImage out;
    if (node.format == MPV_FORMAT_NODE_MAP) {
        int w = 0, h = 0, stride = 0;
        QByteArray bytes;
        QByteArray fmtStr;   // mpv: "bgr0" | "bgra" | "rgba" | "rgb0"
        for (int i = 0; i < node.u.list->num; ++i) {
            const QByteArray key = node.u.list->keys[i];
            const mpv_node& v    = node.u.list->values[i];
            if      (key == "w"      && v.format == MPV_FORMAT_INT64)      w      = int(v.u.int64);
            else if (key == "h"      && v.format == MPV_FORMAT_INT64)      h      = int(v.u.int64);
            else if (key == "stride" && v.format == MPV_FORMAT_INT64)      stride = int(v.u.int64);
            else if (key == "format" && v.format == MPV_FORMAT_STRING)     fmtStr = v.u.string;
            else if (key == "data"   && v.format == MPV_FORMAT_BYTE_ARRAY)
                bytes = QByteArray(reinterpret_cast<const char*>(v.u.ba->data),
                                   int(v.u.ba->size));
        }
        if (w > 0 && h > 0 && stride > 0 && !bytes.isEmpty()) {
            // Map mpv pixel layout → QImage format. Qt RGB32 on little-endian
            // is laid out as B,G,R,X in memory which matches "bgr0".
            QImage::Format qfmt = QImage::Format_RGB32;
            bool swapRB = false;
            if      (fmtStr == "bgr0") { qfmt = QImage::Format_RGB32;          }
            else if (fmtStr == "bgra") { qfmt = QImage::Format_ARGB32;         }
            else if (fmtStr == "rgb0") { qfmt = QImage::Format_RGB32;  swapRB = true; }
            else if (fmtStr == "rgba") { qfmt = QImage::Format_ARGB32; swapRB = true; }
            else                       { qfmt = QImage::Format_RGB32;          }

            QImage img(reinterpret_cast<const uchar*>(bytes.constData()),
                       w, h, stride, qfmt);
            out = img.copy();          // detach: backing buffer dies with `node`
            if (swapRB) out = std::move(out).rgbSwapped();
        }
    }
    mpv_free_node_contents(&node);
    return out;
}

} // namespace promp
