#include "TranscriptionDialog.h"

#include "SubtitleExport.h"
#include "ModelManager.h"
#include "TranscriptionJob.h"
#include "VadParams.h"
#include "WhisperEngine.h"

#include "core/MpvPlayer.h"

#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QHBoxLayout>
#include <QProcess>
#include <QTime>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace promp::ai {

TranscriptionDialog::TranscriptionDialog(promp::MpvPlayer* player, QWidget* parent)
    : QDialog(parent), m_player(player), m_mm(new ModelManager(this)) {
    setWindowTitle(tr("AI 字幕生成"));
    resize(640, 540);

    auto* root = new QVBoxLayout(this);

    // ------- Model group ----------------------------------------------------
    auto* gbModel = new QGroupBox(tr("模型"), this);
    auto* gbm = new QFormLayout(gbModel);
    m_cbModel = new QComboBox(this);
    populateModels();
    gbm->addRow(tr("选择模型:"), m_cbModel);

    auto* statusRow = new QHBoxLayout;
    m_lblModelStatus = new QLabel(this);
    m_btnDownload    = new QPushButton(tr("下载模型"), this);
    statusRow->addWidget(m_lblModelStatus, 1);
    statusRow->addWidget(m_btnDownload, 0);
    gbm->addRow(tr("状态:"), statusRow);
    root->addWidget(gbModel);

    // ------- Decoding group -------------------------------------------------
    auto* gbDec = new QGroupBox(tr("解码"), this);
    auto* gbd = new QFormLayout(gbDec);
    m_cbLanguage = new QComboBox(this);
    m_cbLanguage->addItem(tr("日语"),    QStringLiteral("ja"));   // default
    m_cbLanguage->addItem(tr("自动检测"), QStringLiteral("auto"));
    m_cbLanguage->addItem(tr("中文"),    QStringLiteral("zh"));
    m_cbLanguage->addItem(tr("英语"),    QStringLiteral("en"));
    m_cbLanguage->addItem(tr("韩语"),    QStringLiteral("ko"));
    m_cbLanguage->addItem(tr("法语"),    QStringLiteral("fr"));
    m_cbLanguage->addItem(tr("德语"),    QStringLiteral("de"));
    m_cbLanguage->addItem(tr("西班牙语"), QStringLiteral("es"));
    m_cbLanguage->addItem(tr("俄语"),    QStringLiteral("ru"));
    gbd->addRow(tr("源语言:"), m_cbLanguage);

    m_chkTranslate = new QCheckBox(tr("将识别结果翻译为英语（whisper 自带）"), this);
    m_chkTranslate->setVisible(false);

    m_chkUseGpu = new QCheckBox(tr("使用 GPU 推理（若可用）"), this);
    m_chkUseGpu->setChecked(true);
    gbd->addRow(QString(), m_chkUseGpu);

    m_chkVocalSep = new QCheckBox(tr("预处理：人声分离（BS-RoFormer）"), this);
    m_chkVocalSep->setChecked(false);
    m_chkVocalSep->setToolTip(tr(
        "在 Whisper 转写前先用 BS-RoFormer 模型分离出人声轨道。\n\n"
        "效果：\n"
        "  · 大幅减少背景音乐 / 噪声引起的幻觉复读\n"
        "  · 提高轻声 / 呻吟间台词的识别率\n"
        "  · 让 VAD 在分离后的人声上工作更准确\n\n"
        "首次使用会自动下载模型（~1.2 GB）和依赖包（~500 MB），"
        "后续重复使用走缓存。分离过程约需 30 秒–2 分钟。"));
    gbd->addRow(QString(), m_chkVocalSep);

    m_chkAutoTranslate = new QCheckBox(tr("完成后自动调用 LLM 翻译为目标语言"), this);
    m_chkAutoTranslate->setChecked(true);
    m_chkAutoTranslate->setToolTip(tr(
        "需要先在 “AI 字幕 → 翻译字幕…” 里配好 endpoint / API Key / 模型，"
        "并点过一次 “保存为默认设置”。\n"
        "勾上后转写完会用这套配置自动连跑一次翻译，生成双语 SRT。"));
    gbd->addRow(QString(), m_chkAutoTranslate);

    m_chkUseVad = new QCheckBox(tr("使用 Silero VAD"), this);
    m_chkUseVad->setChecked(true);
    m_chkUseVad->setToolTip(tr(
        "开启后会用 Silero VAD 分析音频。具体用途由下面的'VAD 模式'决定：\n"
        "  · 过滤音频    —— 把静音段裁掉再喂给 whisper（适合干净对话）\n"
        "  · 时间戳校准  —— whisper 看完整音频，VAD 只用于事后修正"
        "时间戳漂移（适合 ASMR / 轻声 / 翻译型 fine-tune）"));
    gbd->addRow(QString(), m_chkUseVad);

    // VAD 用途. Filter is whisper.cpp's classical VAD-as-audio-filter
    // behaviour. Anchor invokes Silero VAD only to nudge cue start
    // times — useful when content is mostly soft voice that VAD might
    // mis-classify (any aggressive filter would drop half the audio).
    m_cbVadMode = new QComboBox(this);
    m_cbVadMode->addItem(tr("时间戳校准（不裁剪音频，仅修正漂移）"),
                         QStringLiteral("anchor"));
    m_cbVadMode->addItem(tr("过滤音频（仅 VAD 检出的语音段进入推理）"),
                         QStringLiteral("filter"));
    m_cbVadMode->setToolTip(tr(
        "VAD 的两种用法 ——\n\n"
        "  时间戳校准 (anchor)：whisper 处理完整音频，不丢任何内容；"
        "之后用 VAD 检出的语音起点来对齐字幕时间。"
        "对翻译型 fine-tune（海南鸡）和轻声 / ASMR 内容最稳。\n\n"
        "  过滤音频 (filter)：VAD 先把语音段挑出来，whisper 只处理"
        "这些段。能消除大段静音 / 音乐 / 噪声带来的幻觉复读，"
        "但对轻声 / 持续呻吟类内容会漏字幕。"));
    gbd->addRow(tr("VAD 模式:"), m_cbVadMode);

    // VAD sensitivity preset — exposes Silero's threshold + padding + silence
    // tolerance as a single user-facing choice. Numeric values per preset live
    // in vadParamsForPreset() so onStartClicked can read them out at runtime.
    m_cbVadPreset = new QComboBox(this);
    m_cbVadPreset->addItem(tr("极宽松（ASMR / 轻声 / 低音量）"), QStringLiteral("ultra"));
    m_cbVadPreset->addItem(tr("宽松（默认 · 一般人声）"),         QStringLiteral("loose"));
    m_cbVadPreset->addItem(tr("标准（清晰对话 / 访谈）"),          QStringLiteral("std"));
    m_cbVadPreset->addItem(tr("严格（嘈杂背景 / 强人声）"),        QStringLiteral("strict"));
    m_cbVadPreset->setToolTip(tr(
        "VAD 灵敏度档位 —— 越宽松，越多音频被认为是'人声'喂给 Whisper：\n\n"
        "  极宽松  threshold=0.10  pad=300ms  min_silence=600ms\n"
        "         （能漏掉的极少；适合轻声细语、ASMR、低音量录制）\n"
        "  宽松    threshold=0.20  pad=200ms  min_silence=400ms\n"
        "         （我们的旧默认；适合一般对话）\n"
        "  标准    threshold=0.35  pad=150ms  min_silence=300ms\n"
        "         （faster-whisper 风格；适合清晰录音）\n"
        "  严格    threshold=0.55  pad=100ms  min_silence=200ms\n"
        "         （嘈杂背景中只挑出明显的强人声）\n\n"
        "如果开 VAD 后发现'许多字幕都消失了'，选'极宽松'再试一次。"));
    gbd->addRow(tr("VAD 灵敏度:"), m_cbVadPreset);

    m_edPrompt = new QLineEdit(this);
    m_edPrompt->setPlaceholderText(tr("可选：人名 / 术语提示，引导识别（短句）"));
    gbd->addRow(tr("提示词:"), m_edPrompt);
    root->addWidget(gbDec);

    // ------- Progress group -------------------------------------------------
    auto* gbProg = new QGroupBox(tr("进度"), this);
    auto* gbp = new QVBoxLayout(gbProg);
    m_lblStage = new QLabel(tr("就绪。"), this);
    m_pbar = new QProgressBar(this);
    m_pbar->setRange(0, 100);
    m_log  = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(300);
    gbp->addWidget(m_lblStage);
    gbp->addWidget(m_pbar);
    gbp->addWidget(m_log, 1);
    root->addWidget(gbProg, 1);

    // ------- Button row -----------------------------------------------------
    m_buttons = new QDialogButtonBox(this);
    m_btnStart  = m_buttons->addButton(tr("开始生成"), QDialogButtonBox::AcceptRole);
    m_btnCancel = m_buttons->addButton(tr("取消"), QDialogButtonBox::RejectRole);
    auto* btnClose = m_buttons->addButton(QDialogButtonBox::Close);
    btnClose->setText(tr("关闭"));
    root->addWidget(m_buttons);

    connect(m_cbModel,     QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &TranscriptionDialog::onModelChanged);
    connect(m_btnDownload, &QPushButton::clicked,
            this, &TranscriptionDialog::onDownloadOrUseClicked);
    connect(m_btnStart,    &QPushButton::clicked,
            this, &TranscriptionDialog::onStartClicked);
    connect(m_btnCancel,   &QPushButton::clicked,
            this, &TranscriptionDialog::onCancelClicked);
    connect(btnClose,      &QPushButton::clicked, this, &QDialog::reject);

    connect(m_mm, &ModelManager::downloadProgress,
            this, &TranscriptionDialog::onDownloadProgress);
    connect(m_mm, &ModelManager::statusChanged,
            this, &TranscriptionDialog::onDownloadStatus);
    connect(m_mm, &ModelManager::downloadFinished,
            this, &TranscriptionDialog::onDownloadFinished);

    m_btnCancel->setEnabled(false);

    // Sticky settings: model selection (handled in populateModels via
    // QSettings), source language, auto-translate flag, and VAD flag.
    {
        QSettings s;
        m_chkAutoTranslate->setChecked(
            s.value(QStringLiteral("transcription/autoTranslate"), true).toBool());
        m_chkUseVad->setChecked(
            s.value(QStringLiteral("transcription/useVad"), true).toBool());
        m_chkVocalSep->setChecked(
            s.value(QStringLiteral("transcription/vocalSeparation"), false).toBool());
        const QString lang = s.value(QStringLiteral("transcription/language"),
                                     QStringLiteral("ja")).toString();
        for (int i = 0; i < m_cbLanguage->count(); ++i) {
            if (m_cbLanguage->itemData(i).toString() == lang) {
                m_cbLanguage->setCurrentIndex(i);
                break;
            }
        }
    }
    connect(m_chkAutoTranslate, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("transcription/autoTranslate"), on);
    });
    connect(m_chkVocalSep, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("transcription/vocalSeparation"), on);
    });
    connect(m_chkUseVad, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("transcription/useVad"), on);
    });
    // VAD preset + mode are sticky. Default mode "anchor" because it's the
    // less destructive choice — works on more content types out of the box.
    {
        QSettings s;
        const QString preset = s.value(QStringLiteral("transcription/vadPreset"),
                                       QStringLiteral("loose")).toString();
        for (int i = 0; i < m_cbVadPreset->count(); ++i) {
            if (m_cbVadPreset->itemData(i).toString() == preset) {
                m_cbVadPreset->setCurrentIndex(i);
                break;
            }
        }
        const QString mode = s.value(QStringLiteral("transcription/vadMode"),
                                     QStringLiteral("anchor")).toString();
        for (int i = 0; i < m_cbVadMode->count(); ++i) {
            if (m_cbVadMode->itemData(i).toString() == mode) {
                m_cbVadMode->setCurrentIndex(i);
                break;
            }
        }
    }
    connect(m_cbVadPreset, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        QSettings().setValue(QStringLiteral("transcription/vadPreset"),
                             m_cbVadPreset->currentData().toString());
    });
    connect(m_cbVadMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        QSettings().setValue(QStringLiteral("transcription/vadMode"),
                             m_cbVadMode->currentData().toString());
    });
    connect(m_cbLanguage, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        QSettings().setValue(QStringLiteral("transcription/language"),
                             m_cbLanguage->currentData().toString());
    });

    // Apply per-model VAD lock for the initially-selected entry. populateModels()
    // ran before our currentIndexChanged hookup, so onModelChanged wasn't called
    // for the startup selection — do it explicitly now so a freshly-opened
    // dialog with ChickenRice selected immediately shows VAD locked-on.
    onModelChanged();
}

TranscriptionDialog::~TranscriptionDialog() {
    if (m_jobActive && m_job)        m_job->requestCancel();
    if (m_translateJob)              m_translateJob->requestCancel();
}

// ---------------------------------------------------------------------------

void TranscriptionDialog::populateModels() {
    m_cbModel->clear();
    for (const auto& d : ModelManager::catalogue()) {
        if (d.isVadModel) continue; // VAD is managed implicitly, not user-facing
        m_cbModel->addItem(d.description, d.id);
    }

    // Preferred selection order:
    //   1) Whatever the user picked last time (persisted to QSettings).
    //   2) The largest already-installed model (so the "default" never
    //      forces another multi-GB download).
    //   3) The catalogue-canonical default ("large-v3-q5_0").
    QSettings s;
    const QString sticky = s.value(QStringLiteral("transcription/modelId")).toString();
    auto trySelect = [&](const QString& id) {
        if (id.isEmpty()) return false;
        for (int i = 0; i < m_cbModel->count(); ++i) {
            if (m_cbModel->itemData(i).toString() == id) {
                m_cbModel->setCurrentIndex(i);
                return true;
            }
        }
        return false;
    };

    if (!trySelect(sticky)) {
        // Walk catalogue from largest to smallest, pick the first that's
        // already on disk. We treat order in the catalogue (large-v3 last)
        // as "biggest = best" — the descriptor's `sizeBytes` is the
        // tie-breaker if we ever shuffle that order.
        QString chosen;
        for (const auto& d : ModelManager::catalogue()) {
            if (d.isVadModel) continue;
            if (m_mm->isInstalled(d.id)) chosen = d.id;
        }
        if (!chosen.isEmpty()) {
            trySelect(chosen);
        } else {
            trySelect(QStringLiteral("large-v3-q5_0"));
        }
    }
}

void TranscriptionDialog::refreshModelStatus() {
    const QString id = m_cbModel->currentData().toString();
    const auto d = ModelManager::descriptorFor(id);
    if (m_mm->isInstalled(id)) {
        // Show the actual on-disk path (may be an alt name like model.bin),
        // not the canonical fileName, so the user can verify what we loaded.
        const QString actual = m_mm->resolveInstalledPath(id);
        m_lblModelStatus->setText(tr("✓ 已安装  (%1)").arg(actual));
        m_btnDownload->setText(tr("打开模型目录"));
        m_btnStart->setEnabled(true);
    } else {
        const double mb = d.sizeBytes / 1'000'000.0;
        m_lblModelStatus->setText(tr("未安装 · 约 %1 MB").arg(QString::number(mb, 'f', 0)));
        m_btnDownload->setText(tr("下载 (%1)").arg(d.id));
        m_btnStart->setEnabled(false);
    }
}

void TranscriptionDialog::onModelChanged() {
    const QString id = m_cbModel->currentData().toString();
    QSettings().setValue(QStringLiteral("transcription/modelId"), id);

    // Per-model hints. Translation fine-tunes (ChickenRice) have known
    // timestamp drift, but the anchor correction system doesn't yet
    // work reliably on breathy/ASMR content — VAD can't distinguish
    // moans from speech, so anchors land on the wrong events and make
    // things worse. Until anchor mode is mature, we just let the user
    // pick whatever VAD setting they want and warn them in the tooltip.
    const auto d = ModelManager::descriptorFor(id);
    m_chkUseVad->setEnabled(true);
    if (d.translatesToZh) {
        m_chkUseVad->setToolTip(tr(
            "此模型为翻译型 fine-tune（海南鸡），时间戳可能有轻微漂移。\n\n"
            "· 关 VAD → 内容完整，时间可能偏差几秒（推荐）\n"
            "· 开 VAD + 过滤音频 → 时间准但会丢掉轻声 / 喘息间的台词\n"
            "· 开 VAD + 时间戳校准 → 实验性功能，在轻声内容上可能反而更乱\n\n"
            "对 ASMR / 轻声内容，推荐关 VAD 保全内容。"));
    } else {
        m_chkUseVad->setToolTip(tr(
            "开启后会用 Silero VAD 分析音频。具体用途由下面的'VAD 模式'决定。\n\n"
            "对包含大段静音 / 音乐的普通对话内容，建议开启'过滤音频'模式。"));
    }

    refreshModelStatus();
}

void TranscriptionDialog::onDownloadOrUseClicked() {
    const QString id = m_cbModel->currentData().toString();
    if (m_mm->isInstalled(id)) {
        // Open the models folder in Explorer.
        QString folder = QDir::toNativeSeparators(m_mm->modelsDir());
        QProcess::startDetached(QStringLiteral("explorer"), {folder});
        return;
    }
    m_btnDownload->setEnabled(false);
    m_btnStart->setEnabled(false);
    m_lblStage->setText(tr("下载模型中…"));
    m_pbar->setRange(0, 100);
    m_pbar->setValue(0);
    m_mm->download(id);
}

void TranscriptionDialog::onDownloadProgress(qint64 received, qint64 total) {
    if (total > 0) {
        m_pbar->setRange(0, 100);
        m_pbar->setValue(int(received * 100 / total));
    } else {
        m_pbar->setRange(0, 0); // indeterminate
    }
    m_lblStage->setText(tr("下载中 %1 / %2 MB")
                           .arg(received / 1'000'000)
                           .arg(total > 0 ? QString::number(total / 1'000'000)
                                          : tr("?")));
}

void TranscriptionDialog::onDownloadStatus(const QString& msg) {
    m_log->appendPlainText(msg);
}

void TranscriptionDialog::onDownloadFinished(const QString&, bool ok, const QString& err) {
    m_btnDownload->setEnabled(true);
    if (!ok) {
        QMessageBox::warning(this, tr("下载失败"), err);
        m_lblStage->setText(tr("下载失败：%1").arg(err));
        m_pbar->setValue(0);
    } else {
        m_lblStage->setText(tr("模型就绪。"));
        m_pbar->setValue(100);
    }
    refreshModelStatus();
}

// ---------------------------------------------------------------------------

void TranscriptionDialog::setUiBusy(bool busy) {
    m_cbModel->setEnabled(!busy);
    m_cbLanguage->setEnabled(!busy);
    m_chkTranslate->setEnabled(!busy);
    m_chkUseGpu->setEnabled(!busy);
    m_chkVocalSep->setEnabled(!busy);
    m_chkUseVad->setEnabled(!busy);
    m_chkAutoTranslate->setEnabled(!busy);
    m_edPrompt->setEnabled(!busy);
    m_btnDownload->setEnabled(!busy);
    m_btnStart->setEnabled(!busy && !m_jobActive
                            && m_mm->isInstalled(m_cbModel->currentData().toString()));
    m_btnCancel->setEnabled(busy);
}

void TranscriptionDialog::onStartClicked() {
    if (!m_player) {
        QMessageBox::warning(this, tr("错误"), tr("播放器未就绪"));
        return;
    }
    const QString media = m_player->getProperty("path").toString();
    if (media.isEmpty()) {
        QMessageBox::information(this, tr("没有视频"),
                                 tr("请先加载一个视频或音频文件。"));
        return;
    }
    const QString id = m_cbModel->currentData().toString();
    if (!m_mm->isInstalled(id)) {
        QMessageBox::information(this, tr("模型未安装"),
                                 tr("请先下载模型。"));
        return;
    }

    // VAD model auto-fetch. Small (~1.8 MB) — we block-await it inline so
    // the user doesn't have to think about it. Reuses the existing
    // ModelManager + dialog progress display; on success we re-enter this
    // slot to actually kick off transcription.
    const QString vadId = QString::fromLatin1(kVadModelId);
    if (m_chkUseVad->isChecked() && !m_mm->isInstalled(vadId)) {
        m_log->appendPlainText(tr("首次使用：下载 Silero VAD 模型…"));
        m_btnStart->setEnabled(false);
        // Wire a one-shot connection that retries Start once download is done.
        auto* conn = new QMetaObject::Connection;
        *conn = connect(m_mm, &ModelManager::downloadFinished, this,
                        [this, conn](const QString& mid, bool ok, const QString& err) {
            if (mid != QString::fromLatin1(kVadModelId)) return;
            QObject::disconnect(*conn);
            delete conn;
            if (!ok) {
                QMessageBox::warning(this, tr("VAD 下载失败"),
                                     tr("将不启用 VAD 继续。\n\n%1").arg(err));
                m_chkUseVad->setChecked(false);
            }
            onStartClicked(); // recurse to actually start
        });
        m_mm->download(vadId);
        return;
    }

    TranscriptionConfig cfg;
    cfg.mediaPath = media;
    // resolveInstalledPath handles alt filenames (e.g. ChickenRice as model.bin).
    cfg.modelPath = m_mm->resolveInstalledPath(id);
    if (cfg.modelPath.isEmpty()) cfg.modelPath = m_mm->pathFor(id);
    if (m_chkUseVad->isChecked() && m_mm->isInstalled(vadId)) {
        cfg.vadModelPath = m_mm->resolveInstalledPath(vadId);
        if (cfg.vadModelPath.isEmpty()) cfg.vadModelPath = m_mm->pathFor(vadId);
    }

    const QFileInfo fi(media);
    const QString outDir = defaultSubtitleExportDir();
    const QString stem = videoStemFromMediaPath(media);
    cfg.outputSrtPath = outDir + QLatin1Char('/')
                       + stem + QStringLiteral("(日语原文).srt");
    cfg.tempDir       = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                       + QStringLiteral("/AuroraPlayer-AI");

    cfg.language           = m_cbLanguage->currentData().toString();
    cfg.translateToEnglish = m_chkTranslate->isChecked();
    cfg.initialPrompt      = m_edPrompt->text().trimmed();
    cfg.gpuDevice          = m_chkUseGpu->isChecked() ? 0 : -1;
    cfg.threads            = 0; // auto
    cfg.vocalSeparation    = m_chkVocalSep->isChecked();
    cfg.vad                = {};
    applyVadPreset(cfg.vad);
    if (m_chkUseVad->isChecked()) {
        const QString mode = m_cbVadMode->currentData().toString();
        cfg.vadMode = (mode == QLatin1String("filter"))
                          ? VadMode::Filter
                          : VadMode::Anchor;  // default Anchor on unknown
    } else {
        cfg.vadMode = VadMode::None;
    }

    // Per-model heuristic: ChickenRice expects Japanese audio → Chinese text.
    if (id == QLatin1String("chickenrice-v2")) {
        if (cfg.language == QLatin1String("auto"))
            cfg.language = QStringLiteral("ja");
        cfg.translateToEnglish = false; // model's own fine-tune outputs zh-CN
    }

    m_job = new TranscriptionJob(cfg, nullptr);
    connect(m_job, &TranscriptionJob::stageChanged,
            this,  &TranscriptionDialog::onJobStage);
    connect(m_job, &TranscriptionJob::progressChanged,
            this,  &TranscriptionDialog::onJobProgress);
    connect(m_job, &TranscriptionJob::newSegment,
            this,  &TranscriptionDialog::onJobSegment);
    connect(m_job, &TranscriptionJob::finished,
            this,  &TranscriptionDialog::onJobFinished);

    m_log->appendPlainText(tr("=== 开始: %1 ===").arg(QFileInfo(media).fileName()));
    m_log->appendPlainText(tr("模型: %1").arg(cfg.modelPath));
    m_log->appendPlainText(tr("输出: %1").arg(cfg.outputSrtPath));
    m_jobActive = true;
    setUiBusy(true);
    m_pbar->setRange(0, 100);
    m_pbar->setValue(0);
    m_job->startAsync();
}

void TranscriptionDialog::onCancelClicked() {
    if (m_job && m_jobActive) {
        m_lblStage->setText(tr("正在取消…"));
        m_job->requestCancel();
    }
}

void TranscriptionDialog::onJobStage(const QString& msg) {
    m_lblStage->setText(msg);
    m_log->appendPlainText(msg);
}

void TranscriptionDialog::onJobProgress(int pct) {
    m_pbar->setRange(0, 100);
    m_pbar->setValue(pct);
}

void TranscriptionDialog::onJobSegment(const WhisperSegment& seg) {
    m_log->appendPlainText(QStringLiteral("[%1] %2")
                              .arg(QTime(0,0).addSecs(int(seg.startSec)).toString("mm:ss"),
                                   seg.text));
}

void TranscriptionDialog::onJobFinished(bool ok, const QString& err, const QString& srt) {
    m_jobActive = false;
    m_job = nullptr;

    if (!ok) {
        setUiBusy(false);
        m_lblStage->setText(tr("失败：%1").arg(err));
        m_log->appendPlainText(tr("=== 失败 ===  %1").arg(err));
        QMessageBox::warning(this, tr("生成失败"), err);
        return;
    }

    m_lblStage->setText(tr("转写完成 → %1").arg(srt));
    m_log->appendPlainText(tr("=== 转写完成: %1 ===").arg(srt));

    // Hand the transcription SRT off to the LLM if the user asked for an
    // end-to-end run. If translation settings aren't present yet we just
    // fall through and load the raw transcription, telling the user how
    // to enable it next time.
    if (m_chkAutoTranslate->isChecked()) {
        if (kickOffAutoTranslation(srt)) {
            // Translation job took over — keep UI busy, do NOT sub-add the
            // raw transcription. The translation completion handler will
            // sub-add the bilingual SRT instead.
            return;
        }
        // Settings missing — log a hint then fall through to default.
        m_log->appendPlainText(
            tr("⚠ 未发现翻译配置（未保存过 endpoint / API Key）。"
               "请到 “AI 字幕 → 翻译字幕…” 里配置一次并点 “保存为默认设置”。"));
    }

    setUiBusy(false);
    if (m_player && QFile::exists(srt)) {
        m_player->sendCommand({QStringLiteral("sub-add"),
                               QDir::toNativeSeparators(srt),
                               QStringLiteral("select")});
    }
    // Stay open so user can read the log / kick off another file.
}

// ---------------------------------------------------------------------------
// Auto-translation pipeline. Reads the QSettings written by TranslationDialog
// and starts a TranslationJob synchronously-on-its-worker-thread; UI stays
// busy until it returns.

bool TranscriptionDialog::kickOffAutoTranslation(const QString& srtPath) {
    QSettings s;
    const QString endpoint = s.value(QStringLiteral("translation/endpoint")).toString();
    const QString apiKey   = s.value(QStringLiteral("translation/apiKey")).toString();
    const QString model    = s.value(QStringLiteral("translation/model")).toString();
    if (endpoint.isEmpty() || apiKey.isEmpty() || model.isEmpty()) {
        return false;
    }

    TranslationJobConfig cfg;
    cfg.sourceSrtPath          = srtPath;
    cfg.engine.endpoint        = endpoint;
    cfg.engine.apiKey          = apiKey;
    cfg.engine.model           = model;
    cfg.engine.targetLanguage  = s.value(QStringLiteral("translation/targetLang"),
                                         QStringLiteral("简体中文")).toString();
    cfg.engine.sourceLanguageHint = s.value(QStringLiteral("translation/sourceLang")).toString();
    cfg.engine.batchSize       = s.value(QStringLiteral("translation/batchSize"), 20).toInt();
    cfg.engine.temperature     = s.value(QStringLiteral("translation/temperature"), 0.20).toDouble();
    cfg.engine.timeoutMs       = s.value(QStringLiteral("translation/timeoutSec"), 60).toInt() * 1000;
    cfg.bilingualLayout        = s.value(QStringLiteral("translation/bilingualLayout"),
                                         QStringLiteral("above")).toString();

    const QString outDir       = defaultSubtitleExportDir();
    const QString videoStem    = videoStemFromSourceSrtForExport(srtPath);
    const QString mainOut      = outDir + QLatin1Char('/') + videoStem + QStringLiteral(".srt");
    const bool    wantBilingual = s.value(QStringLiteral("translation/bilingual"), true).toBool();
    cfg.outputTargetSrtPath     = mainOut;
    // 与目标路径相同时，TranslationJob 先写纯译文再覆盖为双语轨。
    cfg.outputBilingualSrtPath  = wantBilingual ? mainOut : QString();

    m_log->appendPlainText(
        tr("--- 自动翻译: %1 / 目标 %2 ---")
            .arg(model, cfg.engine.targetLanguage));

    m_translateJob = new TranslationJob(cfg, nullptr);
    connect(m_translateJob, &TranslationJob::stageChanged, this,
            [this](const QString& m) {
                m_lblStage->setText(m);
                m_log->appendPlainText(m);
            });
    connect(m_translateJob, &TranslationJob::progressChanged, this,
            [this](int p) { m_pbar->setValue(p); });
    connect(m_translateJob, &TranslationJob::linePreview, this,
            [this](int idx, const QString& src, const QString& tgt) {
                m_log->appendPlainText(QStringLiteral("[%1] %2  →  %3")
                                          .arg(idx + 1)
                                          .arg(src.left(40), tgt.left(40)));
            });
    connect(m_translateJob, &TranslationJob::finished, this,
            [this, srtPath](bool ok, const QString& err,
                            const QString& tgtSrt, const QString& biSrt) {
                m_translateJob = nullptr;
                setUiBusy(false);
                if (!ok) {
                    m_lblStage->setText(tr("翻译失败：%1").arg(err));
                    m_log->appendPlainText(tr("=== 翻译失败 ===  %1").arg(err));
                    QMessageBox::warning(this, tr("翻译失败"), err);
                    // Fall back to loading raw transcription so user still sees
                    // *something*.
                    if (m_player && QFile::exists(srtPath)) {
                        m_player->sendCommand({QStringLiteral("sub-add"),
                                               QDir::toNativeSeparators(srtPath),
                                               QStringLiteral("select")});
                    }
                    return;
                }
                m_lblStage->setText(tr("完成（已加载双语字幕）"));
                m_log->appendPlainText(tr("译文: %1").arg(tgtSrt));
                if (!biSrt.isEmpty())
                    m_log->appendPlainText(tr("双语: %1").arg(biSrt));
                const QString prefer = biSrt.isEmpty() ? tgtSrt : biSrt;
                if (m_player && QFile::exists(prefer)) {
                    m_player->sendCommand({QStringLiteral("sub-add"),
                                           QDir::toNativeSeparators(prefer),
                                           QStringLiteral("select")});
                }
            });

    m_translateJob->startAsync();
    return true;
}

// ---------------------------------------------------------------------------

void TranscriptionDialog::applyVadPreset(VadDecodingParams& vad) const {
    // Numeric values per preset. Keep these in lockstep with the tooltip
    // copy on m_cbVadPreset — if you tweak one, update the other.
    //
    // The pad/silence triplet matters as much as `threshold`: a high
    // threshold with a wide pad still produces broad speech regions
    // (just stricter about *where* a region starts/ends), whereas a
    // low threshold with a tight pad can chop a single utterance into
    // several small regions if there are internal soft pauses.
    const QString id = m_cbVadPreset->currentData().toString();
    if (id == QLatin1String("ultra")) {
        vad.threshold               = 0.10f;
        vad.min_speech_duration_ms  = 80;
        vad.min_silence_duration_ms = 600;
        vad.speech_pad_ms           = 300;
    } else if (id == QLatin1String("std")) {
        vad.threshold               = 0.35f;
        vad.min_speech_duration_ms  = 250;
        vad.min_silence_duration_ms = 300;
        vad.speech_pad_ms           = 150;
    } else if (id == QLatin1String("strict")) {
        vad.threshold               = 0.55f;
        vad.min_speech_duration_ms  = 300;
        vad.min_silence_duration_ms = 200;
        vad.speech_pad_ms           = 100;
    } else {
        // "loose" — our previous hard-coded default.
        vad.threshold               = 0.20f;
        vad.min_speech_duration_ms  = 100;
        vad.min_silence_duration_ms = 400;
        vad.speech_pad_ms           = 200;
    }
    // max_speech_duration_s + all decoding thresholds stay at their
    // VadDecodingParams defaults — those are independent of the
    // sensitivity preset.
}

} // namespace promp::ai
