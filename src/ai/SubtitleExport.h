#pragma once

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QString>

namespace promp::ai {

/// 默认字幕导出目录（可通过 QSettings 键 subtitles/exportDir 覆盖）。
inline QString defaultSubtitleExportDir()
{
    static const QString kDefault =
        QStringLiteral(R"(D:\360安全浏览器下载\gallery-dl下载)");
    QSettings s;
    QString d = s.value(QStringLiteral("subtitles/exportDir"), kDefault).toString();
    if (d.isEmpty())
        d = kDefault;
    QDir().mkpath(d);
    return QDir::cleanPath(d);
}

inline QString sanitizeSubtitleFileStem(QString stem)
{
    const QLatin1Char bad[] = {
        QLatin1Char('<'),  QLatin1Char('>'),  QLatin1Char(':'), QLatin1Char('"'),
        QLatin1Char('/'),  QLatin1Char('\\'), QLatin1Char('|'), QLatin1Char('?'),
        QLatin1Char('*'),
    };
    for (QLatin1Char c : bad)
        stem.replace(c, QLatin1Char('_'));
    constexpr QLatin1Char dot('.');
    constexpr QLatin1Char sp(' ');
    while (stem.endsWith(dot) || stem.endsWith(sp))
        stem.chop(1);
    if (stem.isEmpty())
        stem = QStringLiteral("subtitle");
    return stem;
}

/// 从当前视频的完整路径得到安全的主文件名（不含路径、不含扩展名）。
inline QString videoStemFromMediaPath(const QString& mediaPath)
{
    return sanitizeSubtitleFileStem(QFileInfo(mediaPath).completeBaseName());
}

/// 从源日语 SRT 路径推断「影片名」词干：去掉 “(日语原文)” 或旧版 “.prompv” 后缀。
inline QString videoStemFromSourceSrtForExport(const QString& sourceSrtPath)
{
    QFileInfo fi(sourceSrtPath);
    QString base = fi.completeBaseName();
    const QString jaSuffix = QStringLiteral("(日语原文)");
    if (base.endsWith(jaSuffix))
        base.chop(jaSuffix.size());
    else if (base.endsWith(QStringLiteral(".prompv")))
        base.chop(QStringLiteral(".prompv").size());
    return sanitizeSubtitleFileStem(base);
}

} // namespace promp::ai
