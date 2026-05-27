// TranscriptionDialog.h
// Modal dialog that lets the user pick a model + language, then watches
// the transcription job progress live. Closes itself on completion
// (or stays open with an error message on failure).

#pragma once

#include "TranslationJob.h"

#include <QDialog>
#include <QString>

class QComboBox;
class QLineEdit;
class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QPlainTextEdit;
class QDialogButtonBox;

namespace promp {
class MpvPlayer;
}

namespace promp::ai {

class ModelManager;
class TranscriptionJob;
struct WhisperSegment;

class TranscriptionDialog : public QDialog {
    Q_OBJECT
public:
    /// `player` is used to (a) read the currently-loaded file path and audio
    /// track id, and (b) load the resulting .srt via `sub-add` on success.
    /// The dialog does not take ownership.
    explicit TranscriptionDialog(promp::MpvPlayer* player, QWidget* parent = nullptr);
    ~TranscriptionDialog() override;

private slots:
    void onModelChanged();
    void onDownloadOrUseClicked();
    void onStartClicked();
    void onCancelClicked();

    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadStatus(const QString& msg);
    void onDownloadFinished(const QString& modelId, bool ok, const QString& err);

    void onJobStage(const QString& msg);
    void onJobProgress(int pct);
    void onJobSegment(const promp::ai::WhisperSegment& seg);
    void onJobFinished(bool ok, const QString& err, const QString& srtPath);

private:
    void populateModels();
    void refreshModelStatus();
    void setUiBusy(bool busy);

    promp::MpvPlayer* m_player = nullptr;

    QComboBox*       m_cbModel        = nullptr;
    QLabel*          m_lblModelStatus = nullptr;
    QPushButton*     m_btnDownload    = nullptr;
    QComboBox*       m_cbLanguage     = nullptr;
    QCheckBox*       m_chkTranslate   = nullptr;
    QLineEdit*       m_edPrompt       = nullptr;
    QCheckBox*       m_chkUseGpu      = nullptr;
    QCheckBox*       m_chkVocalSep    = nullptr;
    QCheckBox*       m_chkUseVad      = nullptr;
    QComboBox*       m_cbVadMode      = nullptr;
    QComboBox*       m_cbVadPreset    = nullptr;
    QCheckBox*       m_chkAutoTranslate = nullptr;

    QProgressBar*    m_pbar           = nullptr;
    QLabel*          m_lblStage       = nullptr;
    QPlainTextEdit*  m_log            = nullptr;

    QPushButton*     m_btnStart       = nullptr;
    QPushButton*     m_btnCancel      = nullptr;
    QDialogButtonBox* m_buttons       = nullptr;

    ModelManager*    m_mm  = nullptr;
    TranscriptionJob* m_job = nullptr;
    TranslationJob*  m_translateJob = nullptr;
    bool             m_jobActive = false;

    /// Starts an LLM translation pass on `srtPath` using settings stored
    /// by TranslationDialog (QSettings under "translation/*"). Returns
    /// false if settings are missing — caller should fall back to "done".
    bool kickOffAutoTranslation(const QString& srtPath);

    /// Mutate `vad` per the currently-selected VAD sensitivity preset
    /// in `m_cbVadPreset`. Centralises the threshold/pad/silence triplets
    /// so the dropdown labels and the inference values stay in lockstep.
    void applyVadPreset(struct VadDecodingParams& vad) const;
};

} // namespace promp::ai
