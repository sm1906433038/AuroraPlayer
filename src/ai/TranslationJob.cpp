#include "TranslationJob.h"

#include "SubtitleStream.h"   // formatSrtTimestamp + WhisperSegment

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QStringConverter>

namespace promp::ai {

// ---------------------------------------------------------------------------
// Minimal SRT parser. We only need: index, time range, text payload.
// Multi-line text payloads are joined by " / " into one logical string so
// the LLM gets it as a single line; this keeps the line-count contract
// between request and response.

namespace {

struct ParsedCue {
    int     index = 0;
    double  startSec = 0.0;
    double  endSec = 0.0;
    QString text;  // multi-line joined with " / "
};

double parseSrtTimestamp(const QString& s) {
    // hh:mm:ss,mmm  (also tolerate "." as the ms separator)
    static const QRegularExpression re(
        QStringLiteral("(\\d+):(\\d+):(\\d+)[,\\.](\\d+)"));
    const auto m = re.match(s);
    if (!m.hasMatch()) return 0.0;
    return m.captured(1).toInt() * 3600.0
         + m.captured(2).toInt() * 60.0
         + m.captured(3).toInt()
         + m.captured(4).toInt() / 1000.0;
}

QVector<ParsedCue> parseSrt(const QString& path, QString* errOut) {
    QVector<ParsedCue> cues;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut) *errOut = QStringLiteral("Cannot open: %1").arg(path);
        return cues;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    QString all = ts.readAll();
    all.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));

    // Split on blank-line block boundaries.
    const auto blocks = all.split(QRegularExpression(QStringLiteral("\n\\s*\n")),
                                  Qt::SkipEmptyParts);
    static const QRegularExpression timeLine(
        QStringLiteral("(\\d+:\\d+:\\d+[,\\.]\\d+)\\s*-->\\s*(\\d+:\\d+:\\d+[,\\.]\\d+)"));

    for (const auto& blk : blocks) {
        const auto lines = blk.split(QChar('\n'), Qt::SkipEmptyParts);
        if (lines.size() < 2) continue;

        ParsedCue c;
        int line0 = 0;
        bool ok = false;
        const int firstNum = lines[0].trimmed().toInt(&ok);
        if (ok) { c.index = firstNum; line0 = 1; }

        if (line0 >= lines.size()) continue;
        const auto m = timeLine.match(lines[line0]);
        if (!m.hasMatch()) continue;
        c.startSec = parseSrtTimestamp(m.captured(1));
        c.endSec   = parseSrtTimestamp(m.captured(2));

        QStringList textLines;
        for (int i = line0 + 1; i < lines.size(); ++i)
            textLines << lines[i].trimmed();
        c.text = textLines.join(QStringLiteral(" / "));
        if (c.text.isEmpty()) continue;

        cues.push_back(c);
    }
    return cues;
}

QString srtTime(double s) {
    return formatSrtTimestamp(s);
}

bool writeSrt(const QString& path,
              const QVector<ParsedCue>& cues,
              const QStringList& altTextOrEmpty,
              const QString& bilingualLayout,
              QString* errOut) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errOut) *errOut = QStringLiteral("Cannot write: %1").arg(path);
        return false;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts.setGenerateByteOrderMark(true);

    const bool dual = !altTextOrEmpty.isEmpty();
    for (int i = 0; i < cues.size(); ++i) {
        const auto& c = cues[i];
        ts << (i + 1) << "\r\n"
           << srtTime(c.startSec) << " --> " << srtTime(c.endSec) << "\r\n";
        if (!dual) {
            ts << c.text << "\r\n\r\n";
        } else {
            const QString& alt = altTextOrEmpty.at(i);
            if (bilingualLayout == QLatin1String("below")) {
                ts << c.text << "\r\n" << alt << "\r\n\r\n";
            } else {
                ts << alt << "\r\n" << c.text << "\r\n\r\n";
            }
        }
    }
    ts.flush();
    if (!f.commit()) {
        if (errOut) *errOut = QStringLiteral("Failed to commit: %1").arg(path);
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

TranslationJob::TranslationJob(TranslationJobConfig cfg, QObject* parent)
    : QObject(parent), m_cfg(std::move(cfg)) {}

TranslationJob::~TranslationJob() = default;

void TranslationJob::startAsync() {
    m_thread = new QThread;
    moveToThread(m_thread);
    connect(m_thread, &QThread::started, this, &TranslationJob::run);
    connect(this, &TranslationJob::finished, m_thread, &QThread::quit);
    connect(m_thread, &QThread::finished, this, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
}

void TranslationJob::requestCancel() {
    m_cancel.store(true);
}

void TranslationJob::run() {
    emit stageChanged(tr("解析源 SRT…"));
    QString perr;
    auto cues = parseSrt(m_cfg.sourceSrtPath, &perr);
    if (cues.isEmpty()) {
        emit finished(false,
                      perr.isEmpty()
                          ? tr("源 SRT 解析后没有条目: %1").arg(m_cfg.sourceSrtPath)
                          : perr,
                      {}, {});
        return;
    }
    emit stageChanged(tr("共 %1 条字幕，开始翻译…").arg(cues.size()));

    // The TranslationEngine owns its own QNetworkAccessManager, which must
    // live on the same thread that drives its event loop — we already are
    // on the worker thread when run() executes, so this is correct.
    TranslationEngine engine(m_cfg.engine);

    QStringList translations(cues.size());
    const int batch = qMax(1, m_cfg.engine.batchSize);
    int done = 0;
    for (int start = 0; start < cues.size(); start += batch) {
        if (m_cancel.load()) {
            emit finished(false, tr("已取消"), {}, {});
            return;
        }
        const int end = qMin(cues.size(), start + batch);
        QStringList in;
        for (int i = start; i < end; ++i) in << cues[i].text;

        emit stageChanged(tr("翻译批次 %1 / %2 (%3 条)…")
                              .arg(start / batch + 1)
                              .arg((cues.size() + batch - 1) / batch)
                              .arg(in.size()));

        auto res = engine.translateBatch(in, m_cancel);
        if (!res.ok) {
            emit finished(false,
                          tr("翻译失败（HTTP=%1）: %2")
                              .arg(res.httpStatus).arg(res.error),
                          {}, {});
            return;
        }
        for (int i = 0; i < res.translations.size() && (start + i) < cues.size(); ++i) {
            translations[start + i] = res.translations[i];
            emit linePreview(start + i, in.value(i), res.translations[i]);
        }
        done = end;
        emit progressChanged(int(done * 100.0 / cues.size()));
    }

    // -------------------- write outputs -------------------------------------
    emit stageChanged(tr("写入文件…"));
    QFileInfo srcFi(m_cfg.sourceSrtPath);
    const QString stem = srcFi.absolutePath() + QLatin1Char('/')
                       + srcFi.completeBaseName();
    QString targetPath = m_cfg.outputTargetSrtPath.isEmpty()
                         ? stem + QStringLiteral(".translated.srt")
                         : m_cfg.outputTargetSrtPath;
    QString bilingualPath = m_cfg.outputBilingualSrtPath;
    // The "translated.srt" path replaces each cue's text with the translation:
    QVector<ParsedCue> targetCues = cues;
    for (int i = 0; i < targetCues.size(); ++i)
        targetCues[i].text = translations.value(i);

    QString werr;
    if (!writeSrt(targetPath, targetCues, {}, {}, &werr)) {
        emit finished(false, werr, {}, {});
        return;
    }
    if (!bilingualPath.isEmpty()) {
        if (!writeSrt(bilingualPath, cues, translations,
                      m_cfg.bilingualLayout, &werr)) {
            emit finished(false, werr, targetPath, {});
            return;
        }
    }

    emit progressChanged(100);
    emit finished(true, QString(), targetPath, bilingualPath);
}

} // namespace promp::ai
