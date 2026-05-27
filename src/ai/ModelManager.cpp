#include "ModelManager.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>
#include <QtGlobal>

namespace promp::ai {

// ---------------------------------------------------------------------------
// Catalogue — keep names short. SHA-256 hashes are from HuggingFace's
// official ggerganov/whisper.cpp model card; we record only the ones we
// have actually verified to avoid false-failure rejection if HF reuploads.
// (An empty hash skips integrity check — Network MITM risk is the user's.)
// ---------------------------------------------------------------------------

QList<ModelDescriptor> ModelManager::catalogue() {
    return {
        // ---- large-v3 全精度 ----
        {
            QStringLiteral("large-v3"),
            QStringLiteral("ggml-large-v3.bin"),
            QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3.bin"),
            QString(),
            3'100'000'000,
            QStringLiteral("large-v3 · 通用 · 2.96GB · 最佳质量"),
            true, false,
        },
        // ---- ChickenRice "海南鸡 v2 5000h" 日→中 fine-tune ----
        // HF only hosts SafeTensors + CTranslate2 formats; no ggml is
        // published. To use this model with whisper.cpp you must either:
        //   1) Convert the SafeTensors → ggml locally with whisper.cpp's
        //      `convert-h5-to-ggml.py` (Python + transformers + ~10 min),
        //      then drop the resulting ggml-*.bin into the models folder.
        //   2) Stick with stock large-v3 and translate post-hoc via
        //      DeepSeek / local LLM (Phase 2 in this project).
        // The catalogue entry below recognises a *manually placed* file but
        // won't try to auto-download (URL left blank).
        {
            QStringLiteral("chickenrice-v2"),
            QStringLiteral("ggml-chickenrice-v2-q5_0.bin"),
            QString(),     // no auto-download URL — file must be placed by hand
            QString(),
            1'080'000'000,
            QStringLiteral("海南鸡 v2 · 日→中专用 · 需手动转换/下载"),
            true, true, false,
            // Accept the names community redistributions commonly use:
            //   - model.bin                  (raw HF / CT2 default name)
            //   - ggml-chickenrice-v2.bin    (full-precision conversion)
            //   - ggml-model.bin             (generic whisper.cpp convert script)
            //   - ggml-chickenrice.bin       (older naming)
            QStringList{
                QStringLiteral("model.bin"),
                QStringLiteral("ggml-chickenrice-v2.bin"),
                QStringLiteral("ggml-model.bin"),
                QStringLiteral("ggml-chickenrice.bin"),
            },
        },
        // ---- Silero VAD (used internally; not in the user dropdown) ------
        {
            QStringLiteral("silero-v5"),
            QStringLiteral("ggml-silero-v5.1.2.bin"),
            QStringLiteral("https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v5.1.2.bin"),
            QString(),
            1'800'000,
            QStringLiteral("Silero VAD v5.1.2 · 语音活动检测 · 1.8MB"),
            true, false, true,   // isVadModel = true
        },
    };
}

ModelDescriptor ModelManager::descriptorFor(const QString& id) {
    for (const auto& d : catalogue()) {
        if (d.id == id) return d;
    }
    return {};
}

// ---------------------------------------------------------------------------

ModelManager::ModelManager(QObject* parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

QString ModelManager::modelsDir() const {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dir  = base + QStringLiteral("/models");
    QDir().mkpath(dir);
    return dir;
}

QString ModelManager::pathFor(const ModelDescriptor& d) const {
    return modelsDir() + QLatin1Char('/') + d.fileName;
}
QString ModelManager::pathFor(const QString& id) const {
    return pathFor(descriptorFor(id));
}

QString ModelManager::resolveInstalledPath(const QString& id) const {
    const auto d = descriptorFor(id);
    if (d.id.isEmpty()) return QString();
    const QString dir = modelsDir() + QLatin1Char('/');
    // Canonical name first — this is the one downloads write to.
    const QString primary = dir + d.fileName;
    if (QFileInfo::exists(primary)) return primary;
    // Then any user-provided alternate names (e.g. raw HF "model.bin").
    for (const QString& alt : d.altFileNames) {
        if (alt.isEmpty()) continue;
        const QString p = dir + alt;
        if (QFileInfo::exists(p)) return p;
    }
    return QString();
}

bool ModelManager::isInstalled(const QString& id) const {
    return !resolveInstalledPath(id).isEmpty();
}

bool ModelManager::removeInstalled(const QString& id) {
    const auto d = descriptorFor(id);
    if (d.id.isEmpty()) return false;
    return QFile::remove(pathFor(d));
}

// ---------------------------------------------------------------------------
// Download — single in-flight job. Writes to <file>.part then renames on OK.
// ---------------------------------------------------------------------------

void ModelManager::download(const QString& id) {
    if (m_reply) {
        emit downloadFinished(id, false, tr("已有下载任务在进行"));
        return;
    }
    const auto d = descriptorFor(id);
    if (d.id.isEmpty()) {
        emit downloadFinished(id, false, tr("未知模型 id: %1").arg(id));
        return;
    }
    if (isInstalled(id)) {
        emit downloadFinished(id, true);
        return;
    }
    if (d.url.isEmpty()) {
        emit downloadFinished(id, false,
            tr("此模型不提供自动下载（需手动转换或下载）。"
               "请把转好的 ggml-*.bin 放到模型目录后重试。"));
        return;
    }

    m_currentId   = id;
    m_currentDesc = d;
    m_cancelled   = false;

    const QString tmpPath = pathFor(d) + QStringLiteral(".part");
    m_tmp = new QFile(tmpPath, this);
    if (!m_tmp->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit downloadFinished(id, false, tr("无法创建临时文件: %1").arg(tmpPath));
        delete m_tmp; m_tmp = nullptr;
        return;
    }

    QNetworkRequest req{QUrl(d.url)};
    // Qt 6 follows redirects by default; RedirectPolicyAttribute upgrades
    // the policy to refuse downgrades from https → http (HF uses 302).
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", "AuroraPlayer/0.1 (+whisper-model-fetch)");

    m_reply = m_net->get(req);
    connect(m_reply, &QNetworkReply::readyRead,        this, &ModelManager::onReplyReadyRead);
    connect(m_reply, &QNetworkReply::downloadProgress, this, &ModelManager::onReplyProgress);
    connect(m_reply, &QNetworkReply::finished,         this, &ModelManager::onReplyFinished);

    emit statusChanged(tr("正在下载 %1…").arg(d.fileName));
}

void ModelManager::cancel() {
    m_cancelled = true;
    if (m_reply) m_reply->abort();
}

void ModelManager::onReplyReadyRead() {
    if (m_tmp && m_reply) m_tmp->write(m_reply->readAll());
}

void ModelManager::onReplyProgress(qint64 r, qint64 t) {
    emit downloadProgress(r, t);
}

void ModelManager::onReplyFinished() {
    if (!m_reply) return;
    const QNetworkReply::NetworkError err = m_reply->error();
    if (m_tmp) m_tmp->write(m_reply->readAll());
    m_reply->deleteLater();
    m_reply = nullptr;

    const QString tmpPath = m_tmp ? m_tmp->fileName() : QString();
    if (m_tmp) { m_tmp->close(); delete m_tmp; m_tmp = nullptr; }

    if (m_cancelled) {
        QFile::remove(tmpPath);
        emit downloadFinished(m_currentId, false, tr("下载已取消"));
        m_currentId.clear();
        return;
    }
    if (err != QNetworkReply::NoError) {
        QFile::remove(tmpPath);
        emit downloadFinished(m_currentId, false,
                              tr("网络错误（%1）").arg(int(err)));
        m_currentId.clear();
        return;
    }

    // Optional SHA-256 verify.
    if (!m_currentDesc.sha256.isEmpty()) {
        emit statusChanged(tr("校验文件完整性…"));
        QFile f(tmpPath);
        if (!f.open(QIODevice::ReadOnly)) {
            emit downloadFinished(m_currentId, false, tr("无法打开 .part 文件做校验"));
            return;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&f);
        const QString hex = QString::fromLatin1(hash.result().toHex());
        if (hex.compare(m_currentDesc.sha256, Qt::CaseInsensitive) != 0) {
            QFile::remove(tmpPath);
            emit downloadFinished(m_currentId, false,
                                  tr("SHA-256 不匹配（期望 %1，实际 %2）")
                                    .arg(m_currentDesc.sha256, hex));
            m_currentId.clear();
            return;
        }
    }

    const QString finalPath = pathFor(m_currentDesc);
    QFile::remove(finalPath); // overwrite if any stale half-file exists
    if (!QFile::rename(tmpPath, finalPath)) {
        QFile::remove(tmpPath);
        emit downloadFinished(m_currentId, false, tr("无法落盘到 %1").arg(finalPath));
        m_currentId.clear();
        return;
    }
    emit statusChanged(tr("已就绪：%1").arg(QFileInfo(finalPath).fileName()));
    emit downloadFinished(m_currentId, true);
    m_currentId.clear();
}

} // namespace promp::ai
