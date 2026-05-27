#include "TranslationDialog.h"

#include "SubtitleExport.h"
#include "TranslationJob.h"

#include "core/MpvPlayer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

namespace promp::ai {

namespace {

struct Preset {
    const char* label;
    const char* endpoint;
    const char* defaultModel;
};

// Hard-coded provider presets. "Custom" lets the user paste anything.
// Endpoint URLs all match the OpenAI chat-completions schema.
const Preset kPresets[] = {
    {"DeepSeek",       "https://api.deepseek.com/v1/chat/completions",         "deepseek-chat"},
    {"OpenAI",         "https://api.openai.com/v1/chat/completions",            "gpt-4o-mini"},
    {"Moonshot 月之暗面","https://api.moonshot.cn/v1/chat/completions",          "moonshot-v1-8k"},
    {"智谱 GLM",       "https://open.bigmodel.cn/api/paas/v4/chat/completions", "glm-4-flash"},
    {"通义千问 DashScope","https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions", "qwen-plus"},
    {"自定义",         "",                                                       ""},
};

} // namespace

// ---------------------------------------------------------------------------

TranslationDialog::TranslationDialog(promp::MpvPlayer* player, QWidget* parent)
    : QDialog(parent), m_player(player) {
    setWindowTitle(tr("AI 字幕翻译"));
    resize(720, 620);

    auto* root = new QVBoxLayout(this);

    // ----- Source SRT --------------------------------------------------------
    auto* gbSrc = new QGroupBox(tr("源字幕"), this);
    auto* hbSrc = new QHBoxLayout(gbSrc);
    m_edSourceSrt = new QLineEdit(this);
    m_edSourceSrt->setPlaceholderText(tr("拖入或浏览 .srt 文件"));
    m_btnBrowse = new QPushButton(tr("浏览…"), this);
    hbSrc->addWidget(m_edSourceSrt, 1);
    hbSrc->addWidget(m_btnBrowse, 0);
    root->addWidget(gbSrc);

    // ----- Provider settings -------------------------------------------------
    auto* gbProv = new QGroupBox(tr("API 设置"), this);
    auto* fl = new QFormLayout(gbProv);

    m_cbPreset = new QComboBox(this);
    for (const auto& p : kPresets) m_cbPreset->addItem(QString::fromUtf8(p.label));
    fl->addRow(tr("提供商:"), m_cbPreset);

    m_edEndpoint = new QLineEdit(this);
    m_edEndpoint->setPlaceholderText(
        QStringLiteral("https://api.foo.com/v1/chat/completions（缺尾巴会自动补 /v1/chat/completions）"));
    fl->addRow(tr("Endpoint:"), m_edEndpoint);

    m_edKey = new QLineEdit(this);
    m_edKey->setEchoMode(QLineEdit::Password);
    m_edKey->setPlaceholderText(tr("sk-... (API Key)"));
    fl->addRow(tr("API Key:"), m_edKey);

    m_edModel = new QLineEdit(this);
    m_edModel->setPlaceholderText(QStringLiteral("deepseek-chat / gpt-4o-mini / ..."));
    fl->addRow(tr("模型:"), m_edModel);

    m_cbTargetLang = new QComboBox(this);
    m_cbTargetLang->addItem(QStringLiteral("简体中文"));
    m_cbTargetLang->addItem(QStringLiteral("繁體中文"));
    m_cbTargetLang->addItem(QStringLiteral("English"));
    m_cbTargetLang->addItem(QStringLiteral("日本語"));
    m_cbTargetLang->addItem(QStringLiteral("한국어"));
    fl->addRow(tr("目标语言:"), m_cbTargetLang);

    m_edSourceLang = new QLineEdit(this);
    m_edSourceLang->setPlaceholderText(tr("可空（让模型自检）；例：日语 / Japanese"));
    fl->addRow(tr("源语言提示:"), m_edSourceLang);

    auto* hbAdv = new QHBoxLayout;
    m_spBatch = new QSpinBox(this);
    m_spBatch->setRange(1, 100);
    m_spBatch->setValue(20);
    m_spBatch->setSuffix(tr(" 条/批"));
    m_spTemp = new QDoubleSpinBox(this);
    m_spTemp->setRange(0.0, 1.5);
    m_spTemp->setSingleStep(0.1);
    m_spTemp->setDecimals(2);
    m_spTemp->setValue(0.20);
    m_spTemp->setPrefix(tr("温度 "));
    m_spTimeout = new QSpinBox(this);
    m_spTimeout->setRange(10, 600);
    m_spTimeout->setValue(60);
    m_spTimeout->setSuffix(tr(" 秒超时"));
    m_spTimeout->setToolTip(tr("reasoning 模型（Grok / o1 / DeepSeek-R1）建议 ≥180"));
    hbAdv->addWidget(m_spBatch);
    hbAdv->addWidget(m_spTemp);
    hbAdv->addWidget(m_spTimeout);
    hbAdv->addStretch(1);
    fl->addRow(tr("批处理:"), hbAdv);

    auto* hbBi = new QHBoxLayout;
    m_chkBilingual = new QCheckBox(tr("生成双语字幕"), this);
    m_chkBilingual->setChecked(true);
    m_cbBilingualLayout = new QComboBox(this);
    m_cbBilingualLayout->addItem(tr("译文在上"), QStringLiteral("above"));
    m_cbBilingualLayout->addItem(tr("译文在下"), QStringLiteral("below"));
    hbBi->addWidget(m_chkBilingual);
    hbBi->addWidget(m_cbBilingualLayout);
    hbBi->addStretch(1);
    fl->addRow(tr("双语:"), hbBi);

    root->addWidget(gbProv);

    // ----- Progress ----------------------------------------------------------
    auto* gbProg = new QGroupBox(tr("进度"), this);
    auto* vbProg = new QVBoxLayout(gbProg);
    m_lblStage = new QLabel(tr("就绪。"), this);
    m_pbar     = new QProgressBar(this); m_pbar->setRange(0, 100);
    m_log      = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(500);
    vbProg->addWidget(m_lblStage);
    vbProg->addWidget(m_pbar);
    vbProg->addWidget(m_log, 1);
    root->addWidget(gbProg, 1);

    // ----- Buttons -----------------------------------------------------------
    m_buttons   = new QDialogButtonBox(this);
    m_btnStart  = m_buttons->addButton(tr("开始翻译"),       QDialogButtonBox::AcceptRole);
    m_btnCancel = m_buttons->addButton(tr("取消"),           QDialogButtonBox::RejectRole);
    m_btnSaveDef= m_buttons->addButton(tr("保存为默认设置"), QDialogButtonBox::ActionRole);
    auto* btnClose = m_buttons->addButton(QDialogButtonBox::Close);
    btnClose->setText(tr("关闭"));
    root->addWidget(m_buttons);

    // ----- Wiring ------------------------------------------------------------
    connect(m_cbPreset,  QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &TranslationDialog::onProviderPresetChanged);
    connect(m_btnBrowse, &QPushButton::clicked, this, &TranslationDialog::onBrowseClicked);
    connect(m_btnStart,  &QPushButton::clicked, this, &TranslationDialog::onStartClicked);
    connect(m_btnCancel, &QPushButton::clicked, this, &TranslationDialog::onCancelClicked);
    connect(m_btnSaveDef,&QPushButton::clicked, this, &TranslationDialog::onSaveDefaultsClicked);
    connect(btnClose,    &QPushButton::clicked, this, &QDialog::reject);

    m_btnCancel->setEnabled(false);
    loadDefaults();
}

TranslationDialog::~TranslationDialog() {
    if (m_jobActive && m_job) m_job->requestCancel();
}

// ---------------------------------------------------------------------------

void TranslationDialog::setInitialSourceSrt(const QString& path) {
    m_edSourceSrt->setText(path);
}

void TranslationDialog::onProviderPresetChanged(int idx) {
    if (idx < 0 || idx >= int(sizeof(kPresets) / sizeof(kPresets[0]))) return;
    const auto& p = kPresets[idx];
    const QString endpoint = QString::fromUtf8(p.endpoint);
    const QString model    = QString::fromUtf8(p.defaultModel);
    if (!endpoint.isEmpty()) m_edEndpoint->setText(endpoint);
    if (!model.isEmpty())    m_edModel->setText(model);
}

void TranslationDialog::onBrowseClicked() {
    QString start = QFileInfo(m_edSourceSrt->text()).absolutePath();
    if (start.isEmpty() || !QDir(start).exists())
        start = defaultSubtitleExportDir();
    const QString fn = QFileDialog::getOpenFileName(
        this, tr("选择源 SRT 文件"), start,
        tr("SubRip 字幕 (*.srt);;所有文件 (*)"));
    if (!fn.isEmpty()) m_edSourceSrt->setText(fn);
}

// ---------------------------------------------------------------------------

void TranslationDialog::loadDefaults() {
    QSettings s;
    const int preset = s.value(QStringLiteral("translation/preset"), 0).toInt();
    m_cbPreset->setCurrentIndex(qBound(0, preset, m_cbPreset->count() - 1));
    onProviderPresetChanged(m_cbPreset->currentIndex());

    if (s.contains(QStringLiteral("translation/endpoint")))
        m_edEndpoint->setText(s.value(QStringLiteral("translation/endpoint")).toString());
    if (s.contains(QStringLiteral("translation/apiKey")))
        m_edKey->setText(s.value(QStringLiteral("translation/apiKey")).toString());
    if (s.contains(QStringLiteral("translation/model")))
        m_edModel->setText(s.value(QStringLiteral("translation/model")).toString());
    if (s.contains(QStringLiteral("translation/targetLang"))) {
        const auto v = s.value(QStringLiteral("translation/targetLang")).toString();
        const int idx = m_cbTargetLang->findText(v);
        if (idx >= 0) m_cbTargetLang->setCurrentIndex(idx);
    }
    m_edSourceLang->setText(s.value(QStringLiteral("translation/sourceLang"), QString()).toString());
    m_spBatch->setValue(s.value(QStringLiteral("translation/batchSize"), 20).toInt());
    m_spTemp->setValue(s.value(QStringLiteral("translation/temperature"), 0.20).toDouble());
    m_spTimeout->setValue(s.value(QStringLiteral("translation/timeoutSec"), 60).toInt());
    m_chkBilingual->setChecked(s.value(QStringLiteral("translation/bilingual"), true).toBool());
    const QString layout = s.value(QStringLiteral("translation/bilingualLayout"),
                                   QStringLiteral("above")).toString();
    const int li = m_cbBilingualLayout->findData(layout);
    if (li >= 0) m_cbBilingualLayout->setCurrentIndex(li);
}

void TranslationDialog::saveDefaults() const {
    QSettings s;
    s.setValue(QStringLiteral("translation/preset"),    m_cbPreset->currentIndex());
    s.setValue(QStringLiteral("translation/endpoint"),  m_edEndpoint->text());
    s.setValue(QStringLiteral("translation/apiKey"),    m_edKey->text());
    s.setValue(QStringLiteral("translation/model"),     m_edModel->text());
    s.setValue(QStringLiteral("translation/targetLang"), m_cbTargetLang->currentText());
    s.setValue(QStringLiteral("translation/sourceLang"), m_edSourceLang->text());
    s.setValue(QStringLiteral("translation/batchSize"),  m_spBatch->value());
    s.setValue(QStringLiteral("translation/temperature"),m_spTemp->value());
    s.setValue(QStringLiteral("translation/timeoutSec"), m_spTimeout->value());
    s.setValue(QStringLiteral("translation/bilingual"),  m_chkBilingual->isChecked());
    s.setValue(QStringLiteral("translation/bilingualLayout"),
               m_cbBilingualLayout->currentData().toString());
}

void TranslationDialog::onSaveDefaultsClicked() {
    saveDefaults();
    m_log->appendPlainText(tr("已保存为默认设置（包含 API Key — 仅本机使用！）。"));
}

// ---------------------------------------------------------------------------

void TranslationDialog::setBusy(bool busy) {
    m_btnStart->setEnabled(!busy);
    m_btnCancel->setEnabled(busy);
    m_btnSaveDef->setEnabled(!busy);
    m_btnBrowse->setEnabled(!busy);
    m_cbPreset->setEnabled(!busy);
    m_edEndpoint->setEnabled(!busy);
    m_edKey->setEnabled(!busy);
    m_edModel->setEnabled(!busy);
    m_cbTargetLang->setEnabled(!busy);
    m_edSourceLang->setEnabled(!busy);
    m_spBatch->setEnabled(!busy);
    m_spTemp->setEnabled(!busy);
    m_spTimeout->setEnabled(!busy);
    m_chkBilingual->setEnabled(!busy);
    m_cbBilingualLayout->setEnabled(!busy);
}

void TranslationDialog::onStartClicked() {
    const QString srcSrt = m_edSourceSrt->text().trimmed();
    if (srcSrt.isEmpty() || !QFile::exists(srcSrt)) {
        QMessageBox::information(this, tr("没有源字幕"),
                                 tr("请先指定一个存在的 SRT 文件。"));
        return;
    }

    const QFileInfo fi(srcSrt);
    TranslationJobConfig cfg;
    cfg.sourceSrtPath          = srcSrt;
    cfg.engine.endpoint        = m_edEndpoint->text().trimmed();
    cfg.engine.apiKey          = m_edKey->text().trimmed();
    cfg.engine.model           = m_edModel->text().trimmed();
    cfg.engine.targetLanguage  = m_cbTargetLang->currentText();
    cfg.engine.sourceLanguageHint = m_edSourceLang->text().trimmed();
    cfg.engine.batchSize       = m_spBatch->value();
    cfg.engine.temperature     = m_spTemp->value();
    cfg.engine.timeoutMs       = m_spTimeout->value() * 1000;
    cfg.bilingualLayout        = m_cbBilingualLayout->currentData().toString();

    const QString outDir  = defaultSubtitleExportDir();
    const QString vStem   = videoStemFromSourceSrtForExport(srcSrt);
    const QString mainOut = outDir + QLatin1Char('/') + vStem + QStringLiteral(".srt");
    cfg.outputTargetSrtPath    = mainOut;
    cfg.outputBilingualSrtPath = m_chkBilingual->isChecked() ? mainOut : QString();

    if (cfg.engine.apiKey.isEmpty()) {
        QMessageBox::warning(this, tr("缺少 API Key"),
                             tr("请填写 API Key。"));
        return;
    }

    saveDefaults(); // remember between runs (excluding API key in the future
                    // we can prompt; for now we silently persist).

    m_log->appendPlainText(tr("=== 翻译: %1 ===").arg(fi.fileName()));
    m_log->appendPlainText(tr("Endpoint: %1   模型: %2   目标: %3")
                              .arg(cfg.engine.endpoint, cfg.engine.model,
                                   cfg.engine.targetLanguage));

    m_job = new TranslationJob(cfg, nullptr);
    connect(m_job, &TranslationJob::stageChanged,    this, &TranslationDialog::onJobStage);
    connect(m_job, &TranslationJob::progressChanged, this, &TranslationDialog::onJobProgress);
    connect(m_job, &TranslationJob::linePreview,     this, &TranslationDialog::onJobLinePreview);
    connect(m_job, &TranslationJob::finished,        this, &TranslationDialog::onJobFinished);

    m_jobActive = true;
    setBusy(true);
    m_pbar->setValue(0);
    m_job->startAsync();
}

void TranslationDialog::onCancelClicked() {
    if (m_job && m_jobActive) {
        m_lblStage->setText(tr("正在取消…"));
        m_job->requestCancel();
    }
}

void TranslationDialog::onJobStage(const QString& msg) {
    m_lblStage->setText(msg);
    m_log->appendPlainText(msg);
}
void TranslationDialog::onJobProgress(int p) { m_pbar->setValue(p); }
void TranslationDialog::onJobLinePreview(int idx, const QString& src, const QString& tgt) {
    m_log->appendPlainText(QStringLiteral("[%1] %2  →  %3")
                              .arg(idx + 1).arg(src.left(40), tgt.left(40)));
}

void TranslationDialog::onJobFinished(bool ok, const QString& err,
                                      const QString& targetSrt,
                                      const QString& bilingualSrt) {
    m_jobActive = false;
    m_job = nullptr;
    setBusy(false);

    if (!ok) {
        m_lblStage->setText(tr("失败：%1").arg(err));
        m_log->appendPlainText(tr("=== 失败 ===  %1").arg(err));
        QMessageBox::warning(this, tr("翻译失败"), err);
        return;
    }
    m_lblStage->setText(tr("完成。"));
    m_log->appendPlainText(tr("译文 SRT: %1").arg(targetSrt));
    if (!bilingualSrt.isEmpty())
        m_log->appendPlainText(tr("双语 SRT: %1").arg(bilingualSrt));

    // Auto-load the bilingual (or target) SRT into mpv if the user is currently
    // playing the matching video.
    if (m_player) {
        const QString prefer = bilingualSrt.isEmpty() ? targetSrt : bilingualSrt;
        if (QFile::exists(prefer)) {
            m_player->sendCommand({QStringLiteral("sub-add"),
                                   QDir::toNativeSeparators(prefer),
                                   QStringLiteral("select")});
        }
    }
}

} // namespace promp::ai
