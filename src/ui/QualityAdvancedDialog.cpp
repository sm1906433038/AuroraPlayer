#include "QualityAdvancedDialog.h"
#include "core/MpvPlayer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTabWidget>
#include <QVBoxLayout>

namespace promp {

namespace {

// mpv property names — kept in one place so we can snapshot/restore en masse.
constexpr const char* kHdrProps[] = {
    "tone-mapping", "tone-mapping-param", "target-peak",
    "hdr-peak-percentile", "gamut-mapping-mode", "tone-mapping-max-boost",
};
constexpr const char* kColorProps[] = {
    "brightness", "contrast", "saturation", "gamma", "hue", "sharpen",
};

// Tone-mapping options exposed by mpv. The keys match the property values.
const QList<QPair<QString, QString>>& kToneMappings() {
    static const QList<QPair<QString, QString>> m{
        {QStringLiteral("bt.2446a"), QStringLiteral("bt.2446a (默认，SDR↔HDR 兼顾)")},
        {QStringLiteral("spline"),   QStringLiteral("spline (推荐 miniLED HDR)")},
        {QStringLiteral("bt.2390"),  QStringLiteral("bt.2390 (ITU 参考)")},
        {QStringLiteral("hable"),    QStringLiteral("hable (Uncharted 2 风格)")},
        {QStringLiteral("mobius"),   QStringLiteral("mobius (高光柔和)")},
        {QStringLiteral("reinhard"), QStringLiteral("reinhard (经典)")},
        {QStringLiteral("gamma"),    QStringLiteral("gamma")},
        {QStringLiteral("linear"),   QStringLiteral("linear")},
        {QStringLiteral("clip"),     QStringLiteral("clip (不映射)")},
    };
    return m;
}

const QList<QPair<QString, QString>>& kGamutModes() {
    static const QList<QPair<QString, QString>> m{
        {QStringLiteral("perceptual"), QStringLiteral("perceptual (默认)")},
        {QStringLiteral("relative"),   QStringLiteral("relative")},
        {QStringLiteral("saturation"), QStringLiteral("saturation")},
        {QStringLiteral("absolute"),   QStringLiteral("absolute")},
        {QStringLiteral("desaturate"), QStringLiteral("desaturate")},
        {QStringLiteral("darken"),     QStringLiteral("darken")},
        {QStringLiteral("highlight"),  QStringLiteral("highlight")},
        {QStringLiteral("linear"),     QStringLiteral("linear")},
        {QStringLiteral("clip"),       QStringLiteral("clip")},
    };
    return m;
}

const QList<QPair<QString, QString>>& kTargetPeakOptions() {
    // "auto" lets mpv pick from display metadata; otherwise the value is nits.
    static const QList<QPair<QString, QString>> m{
        {QStringLiteral("auto"), QStringLiteral("自动（推荐）")},
        {QStringLiteral("400"),  QStringLiteral("400 nits (HDR400)")},
        {QStringLiteral("600"),  QStringLiteral("600 nits (HDR600)")},
        {QStringLiteral("800"),  QStringLiteral("800 nits")},
        {QStringLiteral("1000"), QStringLiteral("1000 nits (HDR1000)")},
        {QStringLiteral("1200"), QStringLiteral("1200 nits")},
        {QStringLiteral("1500"), QStringLiteral("1500 nits (miniLED)")},
        {QStringLiteral("2000"), QStringLiteral("2000 nits")},
        {QStringLiteral("4000"), QStringLiteral("4000 nits")},
    };
    return m;
}

const QList<QPair<QString, QString>>& kPercentileOptions() {
    static const QList<QPair<QString, QString>> m{
        {QStringLiteral("100"),    QStringLiteral("100 (精确峰值，可能闪烁)")},
        {QStringLiteral("99.995"), QStringLiteral("99.995 (推荐)")},
        {QStringLiteral("99.9"),   QStringLiteral("99.9 (温和)")},
        {QStringLiteral("99.5"),   QStringLiteral("99.5")},
        {QStringLiteral("99"),     QStringLiteral("99")},
    };
    return m;
}

int findIndex(const QList<QPair<QString, QString>>& m, const QString& value) {
    for (int i = 0; i < m.size(); ++i) {
        if (m[i].first == value) return i;
    }
    return 0;
}

} // namespace

// ---------- ctor / build -----------------------------------------------------

QualityAdvancedDialog::QualityAdvancedDialog(MpvPlayer* player, QWidget* parent)
    : QDialog(parent), m_player(player) {
    setWindowTitle(tr("高级画质设置"));
    resize(560, 600);
    buildUi();
    snapshotForRevert();
    loadFromPlayerAndSettings();
}

void QualityAdvancedDialog::selectTab(int index) {
    if (m_tabs && index >= 0 && index < m_tabs->count()) m_tabs->setCurrentIndex(index);
}

void QualityAdvancedDialog::buildUi() {
    m_tabs = new QTabWidget(this);
    auto* tabs = m_tabs;

    // ---------------- HDR / tone-mapping tab ----------------
    auto* hdrPage = new QWidget(this);
    auto* hdrForm = new QFormLayout(hdrPage);

    m_cbToneMapping = new QComboBox(hdrPage);
    for (const auto& [v, label] : kToneMappings()) m_cbToneMapping->addItem(label, v);
    hdrForm->addRow(tr("色调映射算法"), m_cbToneMapping);

    m_sbToneMappingParam = new QDoubleSpinBox(hdrPage);
    m_sbToneMappingParam->setRange(0.0, 2.0);
    m_sbToneMappingParam->setSingleStep(0.05);
    m_sbToneMappingParam->setDecimals(2);
    m_sbToneMappingParam->setSpecialValueText(tr("auto"));
    hdrForm->addRow(tr("算法参数 (0=auto)"), m_sbToneMappingParam);

    m_cbTargetPeak = new QComboBox(hdrPage);
    for (const auto& [v, label] : kTargetPeakOptions()) m_cbTargetPeak->addItem(label, v);
    hdrForm->addRow(tr("显示器目标峰值亮度"), m_cbTargetPeak);

    m_cbPeakPercentile = new QComboBox(hdrPage);
    for (const auto& [v, label] : kPercentileOptions()) m_cbPeakPercentile->addItem(label, v);
    hdrForm->addRow(tr("HDR 峰值百分位"), m_cbPeakPercentile);

    m_cbGamutMapping = new QComboBox(hdrPage);
    for (const auto& [v, label] : kGamutModes()) m_cbGamutMapping->addItem(label, v);
    hdrForm->addRow(tr("色域映射模式"), m_cbGamutMapping);

    m_sbMaxBoost = new QDoubleSpinBox(hdrPage);
    m_sbMaxBoost->setRange(1.0, 10.0);
    m_sbMaxBoost->setSingleStep(0.1);
    m_sbMaxBoost->setDecimals(2);
    hdrForm->addRow(tr("色调映射最大增益"), m_sbMaxBoost);

    auto* hdrHelp = new QLabel(
        tr("<small>对你的 4K HDR miniLED：建议 <b>spline + 1000~1500 nits + 99.995</b>，"
           "可大幅减少高光区闪烁与丢失细节。<br>"
           "若播 SDR 片源升 HDR 显示，<b>bt.2446a</b> 表现更稳。</small>"),
        hdrPage);
    hdrHelp->setWordWrap(true);
    hdrForm->addRow(hdrHelp);

    tabs->addTab(hdrPage, tr("HDR / 色调映射"));

    // ---------------- Picture-adjust tab ----------------
    auto* picPage = new QWidget(this);
    auto* picV = new QVBoxLayout(picPage);

    auto makeSliderRow = [&](const QString& title, int min, int max, int defVal,
                             QSlider*& outSlider, QLabel*& outVal) {
        auto* row = new QHBoxLayout();
        auto* lbl = new QLabel(title, picPage);
        lbl->setMinimumWidth(80);
        outSlider = new QSlider(Qt::Horizontal, picPage);
        outSlider->setRange(min, max);
        outSlider->setValue(defVal);
        outVal = new QLabel(picPage);
        outVal->setMinimumWidth(48);
        outVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(lbl);
        row->addWidget(outSlider, 1);
        row->addWidget(outVal);
        picV->addLayout(row);
    };

    auto* gbColor = new QGroupBox(tr("色彩"), picPage);
    auto* gbColorV = new QVBoxLayout(gbColor);
    {
        QHBoxLayout* row;
        QLabel* lbl;

        auto rowFor = [&](const QString& title, QSlider*& s, QLabel*& v) {
            row = new QHBoxLayout();
            lbl = new QLabel(title, gbColor);
            lbl->setMinimumWidth(80);
            s = new QSlider(Qt::Horizontal, gbColor);
            s->setRange(-100, 100);
            s->setValue(0);
            v = new QLabel(gbColor);
            v->setMinimumWidth(48);
            v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            row->addWidget(lbl);
            row->addWidget(s, 1);
            row->addWidget(v);
            gbColorV->addLayout(row);
        };
        rowFor(tr("亮度"),   m_slBrightness, m_lblBrightness);
        rowFor(tr("对比度"), m_slContrast,   m_lblContrast);
        rowFor(tr("饱和度"), m_slSaturation, m_lblSaturation);
        rowFor(tr("伽马"),   m_slGamma,      m_lblGamma);
        rowFor(tr("色相"),   m_slHue,        m_lblHue);
    }
    picV->addWidget(gbColor);

    auto* gbSharp = new QGroupBox(tr("锐化 (mpv sharpen)"), picPage);
    auto* gbSharpV = new QVBoxLayout(gbSharp);
    {
        makeSliderRow(tr("强度 x100"), -200, 200, 0, m_slSharpen, m_lblSharpen);
        // mpv `sharpen` is -∞..+∞ but practically -1.0..+1.5; we map slider/100.
        auto* hint = new QLabel(
            tr("<small>0 = 关闭，0.3~0.6 微锐化，0.8~1.2 老片增强，负值为模糊。</small>"),
            gbSharp);
        hint->setWordWrap(true);
        gbSharpV->addWidget(hint);
        gbSharp->setLayout(gbSharpV);
    }
    picV->addWidget(gbSharp);

    auto* gbDenoise = new QGroupBox(tr("降噪 (hqdn3d)"), picPage);
    auto* gbDenoiseV = new QVBoxLayout(gbDenoise);
    {
        m_chkDenoise = new QCheckBox(tr("启用降噪"), gbDenoise);
        gbDenoiseV->addWidget(m_chkDenoise);

        auto* row = new QHBoxLayout();
        auto* lbl = new QLabel(tr("强度"), gbDenoise);
        lbl->setMinimumWidth(80);
        m_slDenoise = new QSlider(Qt::Horizontal, gbDenoise);
        m_slDenoise->setRange(1, 3);
        m_slDenoise->setValue(1);
        m_slDenoise->setTickPosition(QSlider::TicksBelow);
        m_slDenoise->setTickInterval(1);
        m_lblDenoise = new QLabel(gbDenoise);
        m_lblDenoise->setMinimumWidth(48);
        row->addWidget(lbl);
        row->addWidget(m_slDenoise, 1);
        row->addWidget(m_lblDenoise);
        gbDenoiseV->addLayout(row);

        auto* hint = new QLabel(
            tr("<small>1 = 轻度（VHS / 早期数码） · 2 = 中度（DVD / 老蓝光） · 3 = 强（强噪点）。<br>"
               "降噪可能轻微软化画面，与锐化搭配可以兼得。<br>"
               "<b style='color:#ffb84d'>⚠ hqdn3d 是 CPU 滤镜：4K 已较重，8K / VR "
               "会严重卡顿——高分辨率请关闭，或换 GPU 着色器降噪。</b></small>"),
            gbDenoise);
        hint->setWordWrap(true);
        gbDenoiseV->addWidget(hint);
    }
    picV->addWidget(gbDenoise);

    picV->addStretch(1);
    tabs->addTab(picPage, tr("画质调节"));

    // ---------------- buttons ----------------
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                    QDialogButtonBox::Reset, this);
    auto* resetBtn = bb->button(QDialogButtonBox::Reset);
    resetBtn->setText(tr("恢复默认"));
    connect(bb, &QDialogButtonBox::accepted, this, &QualityAdvancedDialog::applyAndAccept);
    connect(bb, &QDialogButtonBox::rejected, this, &QualityAdvancedDialog::revertAndReject);
    connect(resetBtn, &QPushButton::clicked, this, &QualityAdvancedDialog::resetToDefaults);

    auto* outer = new QVBoxLayout(this);
    outer->addWidget(tabs, 1);
    outer->addWidget(bb);

    // -- live-update connections (do this AFTER widgets exist) --
    connect(m_cbToneMapping,     QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){ onToneMappingChanged(); });
    connect(m_sbToneMappingParam,QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double){ onToneMappingParamChanged(); });
    connect(m_cbTargetPeak,      QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){ onTargetPeakChanged(); });
    connect(m_cbPeakPercentile,  QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){ onPeakPercentileChanged(); });
    connect(m_cbGamutMapping,    QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){ onGamutChanged(); });
    connect(m_sbMaxBoost,        QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double){ onMaxBoostChanged(); });

    for (QSlider* s : { m_slBrightness, m_slContrast, m_slSaturation, m_slGamma, m_slHue })
        connect(s, &QSlider::valueChanged, this, &QualityAdvancedDialog::onColorChanged);
    connect(m_slSharpen, &QSlider::valueChanged, this, &QualityAdvancedDialog::onSharpenChanged);
    connect(m_chkDenoise, &QCheckBox::toggled, this, &QualityAdvancedDialog::onDenoiseToggled);
    connect(m_slDenoise,  &QSlider::valueChanged, this, &QualityAdvancedDialog::onDenoiseLevelChanged);
}

// ---------- snapshot / load --------------------------------------------------

void QualityAdvancedDialog::snapshotForRevert() {
    if (!m_player) return;
    auto cap = [&](const char* name) {
        m_snapshot.insert(QByteArray(name), m_player->getProperty(QByteArray(name)));
    };
    for (auto* p : kHdrProps)   cap(p);
    for (auto* p : kColorProps) cap(p);
    m_vfSnapshot = m_player->getProperty("vf").toString();
}

void QualityAdvancedDialog::loadFromPlayerAndSettings() {
    if (!m_player) return;
    const QSignalBlocker b1(m_cbToneMapping);
    const QSignalBlocker b2(m_sbToneMappingParam);
    const QSignalBlocker b3(m_cbTargetPeak);
    const QSignalBlocker b4(m_cbPeakPercentile);
    const QSignalBlocker b5(m_cbGamutMapping);
    const QSignalBlocker b6(m_sbMaxBoost);
    const QSignalBlocker b7(m_slBrightness);
    const QSignalBlocker b8(m_slContrast);
    const QSignalBlocker b9(m_slSaturation);
    const QSignalBlocker b10(m_slGamma);
    const QSignalBlocker b11(m_slHue);
    const QSignalBlocker b12(m_slSharpen);
    const QSignalBlocker b13(m_chkDenoise);
    const QSignalBlocker b14(m_slDenoise);

    QSettings s;

    // HDR
    const QString tm = s.value("hdr/toneMapping",
                               m_player->getProperty("tone-mapping").toString()).toString();
    m_cbToneMapping->setCurrentIndex(findIndex(kToneMappings(), tm));

    const double tmp = s.value("hdr/toneMappingParam",
                               m_player->getProperty("tone-mapping-param").toDouble()).toDouble();
    m_sbToneMappingParam->setValue(tmp);

    const QString tp = s.value("hdr/targetPeak", QStringLiteral("auto")).toString();
    m_cbTargetPeak->setCurrentIndex(findIndex(kTargetPeakOptions(), tp));

    const QString pp = s.value("hdr/peakPercentile", QStringLiteral("99.995")).toString();
    m_cbPeakPercentile->setCurrentIndex(findIndex(kPercentileOptions(), pp));

    const QString gm = s.value("hdr/gamutMapping",
                               m_player->getProperty("gamut-mapping-mode").toString()).toString();
    m_cbGamutMapping->setCurrentIndex(findIndex(kGamutModes(), gm));

    m_sbMaxBoost->setValue(s.value("hdr/maxBoost", 1.0).toDouble());

    // Color
    m_slBrightness->setValue(s.value("pic/brightness", 0).toInt());
    m_slContrast  ->setValue(s.value("pic/contrast",   0).toInt());
    m_slSaturation->setValue(s.value("pic/saturation", 0).toInt());
    m_slGamma     ->setValue(s.value("pic/gamma",      0).toInt());
    m_slHue       ->setValue(s.value("pic/hue",        0).toInt());

    // Sharpen — stored as slider * 100
    m_slSharpen->setValue(s.value("pic/sharpenX100", 0).toInt());

    // Denoise
    m_chkDenoise->setChecked(s.value("pic/denoise", false).toBool());
    m_slDenoise ->setValue (s.value("pic/denoiseLevel", 1).toInt());

    // Push initial state to mpv (in case settings differ from current).
    onToneMappingChanged();
    onToneMappingParamChanged();
    onTargetPeakChanged();
    onPeakPercentileChanged();
    onGamutChanged();
    onMaxBoostChanged();
    onColorChanged();
    onSharpenChanged();
    onDenoiseToggled();   // (re)builds vf string

    // Update value labels.
    m_lblBrightness->setText(QString::number(m_slBrightness->value()));
    m_lblContrast  ->setText(QString::number(m_slContrast->value()));
    m_lblSaturation->setText(QString::number(m_slSaturation->value()));
    m_lblGamma     ->setText(QString::number(m_slGamma->value()));
    m_lblHue       ->setText(QString::number(m_slHue->value()));
    m_lblSharpen   ->setText(QString::asprintf("%+.2f", m_slSharpen->value() / 100.0));
    m_lblDenoise   ->setText(QString::number(m_slDenoise->value()));
}

// ---------- live application -------------------------------------------------

void QualityAdvancedDialog::writeProperty(const char* name, const QVariant& value) {
    if (m_player) m_player->setProperty(QByteArray(name), value);
}
void QualityAdvancedDialog::writePropertyStr(const char* name, const QString& value) {
    writeProperty(name, value);
}

void QualityAdvancedDialog::onToneMappingChanged() {
    writePropertyStr("tone-mapping", m_cbToneMapping->currentData().toString());
}
void QualityAdvancedDialog::onToneMappingParamChanged() {
    writeProperty("tone-mapping-param", m_sbToneMappingParam->value());
}
void QualityAdvancedDialog::onTargetPeakChanged() {
    // mpv accepts "auto" (string) or an integer nits value; pass the raw
    // string so both code paths work without us having to know mpv's
    // current version-specific encoding.
    writePropertyStr("target-peak", m_cbTargetPeak->currentData().toString());
}
void QualityAdvancedDialog::onPeakPercentileChanged() {
    writeProperty("hdr-peak-percentile", m_cbPeakPercentile->currentData().toDouble());
}
void QualityAdvancedDialog::onGamutChanged() {
    writePropertyStr("gamut-mapping-mode", m_cbGamutMapping->currentData().toString());
}
void QualityAdvancedDialog::onMaxBoostChanged() {
    writeProperty("tone-mapping-max-boost", m_sbMaxBoost->value());
}

void QualityAdvancedDialog::onColorChanged() {
    writeProperty("brightness", m_slBrightness->value());
    writeProperty("contrast",   m_slContrast->value());
    writeProperty("saturation", m_slSaturation->value());
    writeProperty("gamma",      m_slGamma->value());
    writeProperty("hue",        m_slHue->value());
    m_lblBrightness->setText(QString::number(m_slBrightness->value()));
    m_lblContrast  ->setText(QString::number(m_slContrast->value()));
    m_lblSaturation->setText(QString::number(m_slSaturation->value()));
    m_lblGamma     ->setText(QString::number(m_slGamma->value()));
    m_lblHue       ->setText(QString::number(m_slHue->value()));
}

void QualityAdvancedDialog::onSharpenChanged() {
    const double v = m_slSharpen->value() / 100.0;
    writeProperty("sharpen", v);
    m_lblSharpen->setText(QString::asprintf("%+.2f", v));
}

void QualityAdvancedDialog::onDenoiseToggled() {
    applyDenoiseFilter();
}
void QualityAdvancedDialog::onDenoiseLevelChanged() {
    m_lblDenoise->setText(QString::number(m_slDenoise->value()));
    applyDenoiseFilter();
}

void QualityAdvancedDialog::applyDenoiseFilter() {
    const QString filter = m_chkDenoise->isChecked()
                              ? denoiseFilterFor(m_slDenoise->value())
                              : QString();
    // We own the `vf` slot during the dialog session.
    writePropertyStr("vf", filter);
}

QString QualityAdvancedDialog::denoiseFilterFor(int strength) {
    // hqdn3d=<luma_spatial>:<chroma_spatial>:<luma_tmp>:<chroma_tmp>
    switch (strength) {
        case 1: return QStringLiteral("hqdn3d=2:1:2:3");
        case 2: return QStringLiteral("hqdn3d=4:3:6:4.5");
        case 3: return QStringLiteral("hqdn3d=6:4:9:6");
        default:return QString();
    }
}

// ---------- accept / reject / reset ------------------------------------------

void QualityAdvancedDialog::applyAndAccept() {
    QSettings s;

    // HDR
    s.setValue("hdr/toneMapping",      m_cbToneMapping->currentData().toString());
    s.setValue("hdr/toneMappingParam", m_sbToneMappingParam->value());
    s.setValue("hdr/targetPeak",       m_cbTargetPeak->currentData().toString());
    s.setValue("hdr/peakPercentile",   m_cbPeakPercentile->currentData().toString());
    s.setValue("hdr/gamutMapping",     m_cbGamutMapping->currentData().toString());
    s.setValue("hdr/maxBoost",         m_sbMaxBoost->value());

    // Color
    s.setValue("pic/brightness",       m_slBrightness->value());
    s.setValue("pic/contrast",         m_slContrast->value());
    s.setValue("pic/saturation",       m_slSaturation->value());
    s.setValue("pic/gamma",            m_slGamma->value());
    s.setValue("pic/hue",              m_slHue->value());
    s.setValue("pic/sharpenX100",      m_slSharpen->value());

    s.setValue("pic/denoise",          m_chkDenoise->isChecked());
    s.setValue("pic/denoiseLevel",     m_slDenoise->value());

    accept();
}

void QualityAdvancedDialog::revertAndReject() {
    // Restore every property captured in the snapshot.
    for (auto it = m_snapshot.constBegin(); it != m_snapshot.constEnd(); ++it) {
        m_player->setProperty(it.key(), it.value());
    }
    m_player->setProperty("vf", m_vfSnapshot);
    reject();
}

void QualityAdvancedDialog::resetToDefaults() {
    // HDR defaults match the High/Ultra preset baseline.
    m_cbToneMapping    ->setCurrentIndex(findIndex(kToneMappings(),    QStringLiteral("bt.2446a")));
    m_sbToneMappingParam->setValue(0.0);
    m_cbTargetPeak     ->setCurrentIndex(findIndex(kTargetPeakOptions(), QStringLiteral("0")));
    m_cbPeakPercentile ->setCurrentIndex(findIndex(kPercentileOptions(), QStringLiteral("99.995")));
    m_cbGamutMapping   ->setCurrentIndex(findIndex(kGamutModes(),       QStringLiteral("perceptual")));
    m_sbMaxBoost       ->setValue(1.0);

    m_slBrightness->setValue(0);
    m_slContrast  ->setValue(0);
    m_slSaturation->setValue(0);
    m_slGamma     ->setValue(0);
    m_slHue       ->setValue(0);
    m_slSharpen   ->setValue(0);
    m_chkDenoise  ->setChecked(false);
    m_slDenoise   ->setValue(1);
}

} // namespace promp
