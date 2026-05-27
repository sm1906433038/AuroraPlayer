// ModelManager.h
// Discovers, downloads, and validates whisper.cpp ggml model files.
//
// Model files live in a writable per-user directory:
//   <AppDataLocation>/AuroraPlayer/models/
// e.g. on Windows: C:/Users/<u>/AppData/Roaming/AuroraPlayer/models/
//
// Each model is identified by a short id ("base" / "small" / "medium" /
// "large-v3" / ...). The file naming follows whisper.cpp convention:
//   ggml-<id>.bin             (full precision)
//   ggml-<id>-q5_0.bin        (q5_0 quantised, ~30% size)
//
// Models are listed below with their HuggingFace download URL and SHA-256.
// First-time use triggers a download into a temp file, validated against
// the recorded hash before being renamed into place. Cancel-safe.

#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

namespace promp::ai {

/// Describes one downloadable model.
struct ModelDescriptor {
    QString id;          ///< short user-facing id ("large-v3-q5_0")
    QString fileName;    ///< on-disk name ("ggml-large-v3-q5_0.bin")
    QString url;         ///< HuggingFace download URL
    QString sha256;      ///< canonical SHA-256 — empty disables integrity check
    qint64  sizeBytes = 0;
    QString description; ///< human label shown in UI
    bool    multilingual = true;
    bool    translatesToZh = false; ///< true for ChickenRice fine-tunes
    bool    isVadModel = false;     ///< Silero VAD — not for the user dropdown
    /// Alternate file names that should also be recognised as "installed".
    /// Useful for community-redistributed fine-tunes whose authors keep the
    /// generic name "model.bin" instead of the ggml-<id>.bin convention.
    QStringList altFileNames;
};

/// Canonical id of the bundled Silero VAD model (used internally by VAD).
inline constexpr const char* kVadModelId = "silero-v5";

class ModelManager : public QObject {
    Q_OBJECT
public:
    explicit ModelManager(QObject* parent = nullptr);

    /// Where model files live. Created on demand.
    [[nodiscard]] QString modelsDir() const;

    /// Catalogue (built-in, hard-coded — these are the models we recommend).
    [[nodiscard]] static QList<ModelDescriptor> catalogue();

    /// Find a descriptor by id (or empty descriptor if unknown).
    [[nodiscard]] static ModelDescriptor descriptorFor(const QString& id);

    /// Absolute path to where a model SHOULD live (regardless of presence).
    [[nodiscard]] QString pathFor(const ModelDescriptor& d) const;
    [[nodiscard]] QString pathFor(const QString& modelId) const;

    /// Path to the *existing* on-disk model file, considering the canonical
    /// fileName plus any altFileNames. Returns an empty string if none of
    /// them exist. Prefer this over pathFor() when actually loading.
    [[nodiscard]] QString resolveInstalledPath(const QString& modelId) const;

    /// True if the model file is on disk under the canonical name or any
    /// recognised alternate name (does not verify hash).
    [[nodiscard]] bool isInstalled(const QString& modelId) const;

    /// Kick off a download. Emits progress / finished. Idempotent — if
    /// the file is already on disk it emits finished(true) immediately.
    void download(const QString& modelId);

    /// Cancel an in-flight download (no-op if none).
    void cancel();

    /// Delete a model file from disk.
    bool removeInstalled(const QString& modelId);

signals:
    /// Bytes received vs. total (total may be -1 if server doesn't advertise).
    void downloadProgress(qint64 received, qint64 total);
    /// Plain-text status line for the UI ("正在下载…", "校验中…", "完成").
    void statusChanged(const QString& msg);
    /// Final event. `ok=true` means the model is now on disk and ready.
    void downloadFinished(const QString& modelId, bool ok,
                          const QString& errorMessage = {});

private slots:
    void onReplyReadyRead();
    void onReplyFinished();
    void onReplyProgress(qint64 received, qint64 total);

private:
    void startNextRedirectIfAny();

    QNetworkAccessManager* m_net = nullptr;
    QNetworkReply*         m_reply = nullptr;
    QFile*                 m_tmp = nullptr;
    QString                m_currentId;
    ModelDescriptor        m_currentDesc;
    bool                   m_cancelled = false;
};

} // namespace promp::ai
