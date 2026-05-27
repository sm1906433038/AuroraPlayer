// TranslationDialog.h
// Modal-ish dialog: pick a source SRT, configure the LLM endpoint / key /
// model, kick off translation, watch progress.

#pragma once

#include <QDialog>
#include <QString>

class QComboBox;
class QLineEdit;
class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QDialogButtonBox;

namespace promp {
class MpvPlayer;
}

namespace promp::ai {

class TranslationJob;

class TranslationDialog : public QDialog {
    Q_OBJECT
public:
    explicit TranslationDialog(promp::MpvPlayer* player, QWidget* parent = nullptr);
    ~TranslationDialog() override;

    /// Pre-fill the source SRT path. Called by MainWindow when launched
    /// right after a transcription job so the user can chain steps without
    /// re-picking a file.
    void setInitialSourceSrt(const QString& path);

private slots:
    void onProviderPresetChanged(int);
    void onBrowseClicked();
    void onStartClicked();
    void onCancelClicked();
    void onSaveDefaultsClicked();

    void onJobStage(const QString& msg);
    void onJobProgress(int pct);
    void onJobLinePreview(int idx, const QString& src, const QString& tgt);
    void onJobFinished(bool ok, const QString& err,
                       const QString& targetSrt,
                       const QString& bilingualSrt);

private:
    void loadDefaults();
    void saveDefaults() const;
    void setBusy(bool busy);

    promp::MpvPlayer* m_player = nullptr;

    QComboBox*   m_cbPreset      = nullptr;
    QLineEdit*   m_edEndpoint    = nullptr;
    QLineEdit*   m_edKey         = nullptr;
    QLineEdit*   m_edModel       = nullptr;
    QComboBox*   m_cbTargetLang  = nullptr;
    QLineEdit*   m_edSourceLang  = nullptr;
    QSpinBox*    m_spBatch       = nullptr;
    QDoubleSpinBox* m_spTemp     = nullptr;
    QSpinBox*    m_spTimeout     = nullptr;
    QCheckBox*   m_chkBilingual  = nullptr;
    QComboBox*   m_cbBilingualLayout = nullptr;

    QLineEdit*   m_edSourceSrt   = nullptr;
    QPushButton* m_btnBrowse     = nullptr;

    QProgressBar*    m_pbar      = nullptr;
    QLabel*          m_lblStage  = nullptr;
    QPlainTextEdit*  m_log       = nullptr;

    QPushButton* m_btnStart      = nullptr;
    QPushButton* m_btnCancel     = nullptr;
    QPushButton* m_btnSaveDef    = nullptr;
    QDialogButtonBox* m_buttons  = nullptr;

    TranslationJob* m_job = nullptr;
    bool            m_jobActive = false;
};

} // namespace promp::ai
