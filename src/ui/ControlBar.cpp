#include "ControlBar.h"
#include "SeekBar.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>

namespace promp {

ControlBar::ControlBar(QWidget* parent) : QWidget(parent) {
    setObjectName("ControlBar");
    buildLayout();
}

QString ControlBar::fmtTime(double s) {
    if (s < 0 || !qIsFinite(s)) s = 0;
    const qint64 totalMs = qint64(s * 1000.0 + 0.5);
    const qint64 ms = totalMs % 1000;
    const qint64 sec = totalMs / 1000;
    const qint64 h  = sec / 3600;
    const qint64 m  = (sec / 60) % 60;
    const qint64 ss = sec % 60;
    return QString::asprintf("%lld:%02lld:%02lld.%03lld", h, m, ss, ms);
}

void ControlBar::buildLayout() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 6, 10, 8);
    outer->setSpacing(4);

    m_seek = new SeekBar(this);
    outer->addWidget(m_seek);

    auto* row = new QHBoxLayout;
    row->setSpacing(4);

    // Glyph-style media buttons using Unicode media symbols. Bigger font +
    // explicit white-on-dark colour so they're clearly visible on the control
    // bar's dark background. Hover gives a subtle highlight.
    static const char* kGlyphSheet =
        "QToolButton {"
        "  font: 18px 'Segoe UI Symbol';"
        "  color: #dfe2e6;"
        "  padding: 2px 8px;"
        "  border: none;"
        "  border-radius: 4px;"
        "  min-width: 28px;"
        "}"
        "QToolButton:hover   { background: #2e2e34; color: #ffffff; }"
        "QToolButton:pressed { background: #1e6cd6; color: #ffffff; }";

    auto makeGlyph = [this](const QString& glyph, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setText(glyph);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setAutoRaise(true);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(kGlyphSheet);
        return b;
    };

    // ⏮ ⏪ ▶ ⏩ ⏭   (and ⏸ ⏹ for state toggles)
    m_btnPrev    = makeGlyph(QStringLiteral("⏮"), tr("上一个视频"));
    m_btnPrevKey = makeGlyph(QStringLiteral("⏪"), tr("上一关键帧 (Ctrl+←)"));
    m_btnPlay    = makeGlyph(QStringLiteral("▶"), tr("播放 / 暂停（空格）"));
    m_btnNextKey = makeGlyph(QStringLiteral("⏩"), tr("下一关键帧 (Ctrl+→)"));
    m_btnNext    = makeGlyph(QStringLiteral("⏭"), tr("下一个视频"));
    m_btnMute    = makeGlyph(QStringLiteral("🔊"), tr("静音（M）"));
    m_btnFull    = makeGlyph(QStringLiteral("⛶"), tr("全屏（F）"));

    // 字幕 / VR — text buttons that pop their own menu on click.
    auto makeTextBtn = [this](const QString& label, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setText(label);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setAutoRaise(true);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setPopupMode(QToolButton::InstantPopup);
        b->setStyleSheet(
            "QToolButton {"
            "  padding: 4px 10px; border-radius: 4px;"
            "  font: 12px 'Segoe UI'; color:#dfe2e6;"
            "}"
            "QToolButton:hover { background:#2e2e34; color:#ffffff; }"
            "QToolButton::menu-indicator { width:0; }");
        return b;
    };
    m_btnSubs = makeTextBtn(tr("字幕"), tr("字幕设置：轨道 / 缩放 / 位置 / 编码"));
    m_btnVr   = makeTextBtn(tr("VR"),   tr("VR / 360° 设置：投影 / 立体格式 / 重置视角"));

    // 续播：记忆本机文件的播放进度（可关闭）
    m_btnResume = makeTextBtn(tr("续播"), tr("开启后，下次打开同一文件从上次停止位置继续播放"));
    m_btnResume->setCheckable(true);
    m_btnResume->setChecked(false);
    m_btnResume->setStyleSheet(
        m_btnResume->styleSheet()
        + QStringLiteral("QToolButton:checked { background:#153d28; color:#8ef0b0; }"));

    m_lblPos = new QLabel("0:00:00.000", this);
    m_lblDur = new QLabel("0:00:00.000", this);
    // h:mm:ss.mmm needs more room than the old m:ss layout.
    m_lblPos->setMinimumWidth(86);
    m_lblDur->setMinimumWidth(86);

    m_volume = new QSlider(Qt::Horizontal, this);
    m_volume->setRange(0, 130);
    m_volume->setValue(100);
    m_volume->setFixedWidth(110);
    // Click anywhere on the groove → jump to that position immediately,
    // instead of the default "page step" behaviour.
    m_volume->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { height: 6px; }"
    ));
    m_volume->installEventFilter(this);

    m_speedBox = new QComboBox(this);
    for (double s : { 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 3.0, 4.0 }) {
        m_speedBox->addItem(QString::number(s, 'g', 3) + "x", s);
    }
    m_speedBox->setCurrentText("1x");

    // ----- Layout ------------------------------------------------------------
    // [time]  ───stretch───  [⏮ ⏪ ▶ ⏩ ⏭]  ───stretch───  [续播 字幕 VR 倍速 …]
    row->addSpacing(8);
    row->addWidget(m_lblPos);
    row->addWidget(new QLabel("/", this));
    row->addWidget(m_lblDur);
    row->addStretch(1);

    row->addWidget(m_btnPrev);
    row->addWidget(m_btnPrevKey);
    row->addWidget(m_btnPlay);
    row->addWidget(m_btnNextKey);
    row->addWidget(m_btnNext);

    row->addStretch(1);

    m_lblMediaInfo = new QLabel(this);
    m_lblMediaInfo->setStyleSheet(
        "QLabel {"
        "  font: 11px 'Segoe UI';"
        "  color: #9da5b0;"
        "  padding: 0 6px;"
        "}");
    m_lblMediaInfo->setToolTip(tr("当前视频分辨率 / 码率"));
    row->addWidget(m_lblMediaInfo);

    row->addWidget(m_btnResume);
    row->addWidget(m_btnSubs);
    row->addWidget(m_btnVr);
    row->addSpacing(4);
    row->addWidget(m_speedBox);
    row->addWidget(m_btnMute);
    row->addWidget(m_volume);
    row->addWidget(m_btnFull);

    outer->addLayout(row);

    // Signal routing
    connect(m_btnPlay,    &QToolButton::clicked, this, &ControlBar::playPauseClicked);
    connect(m_btnPrev,    &QToolButton::clicked, this, &ControlBar::prevClicked);
    connect(m_btnNext,    &QToolButton::clicked, this, &ControlBar::nextClicked);
    connect(m_btnPrevKey, &QToolButton::clicked, this, &ControlBar::prevKeyframeClicked);
    connect(m_btnNextKey, &QToolButton::clicked, this, &ControlBar::nextKeyframeClicked);
    connect(m_btnMute,    &QToolButton::clicked, this, &ControlBar::muteToggled);
    connect(m_btnFull,    &QToolButton::clicked, this, &ControlBar::fullscreenToggled);
    connect(m_btnResume,  &QToolButton::toggled, this, &ControlBar::resumeRememberToggled);
    connect(m_volume,  &QSlider::valueChanged, this,
            [this](int v) { emit volumeChanged(double(v)); });
    connect(m_speedBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { emit speedChanged(m_speedBox->currentData().toDouble()); });
}

void ControlBar::setDuration(double seconds) {
    m_duration = qMax(0.0, seconds);
    m_seek->setDuration(m_duration);
    m_lblDur->setText(fmtTime(m_duration));
}

void ControlBar::setPosition(double seconds) {
    m_seek->setPosition(seconds);
    m_lblPos->setText(fmtTime(seconds));
}

void ControlBar::setPaused(bool paused) {
    m_btnPlay->setText(paused ? QStringLiteral("▶") : QStringLiteral("⏸"));
}

void ControlBar::setVolume(double percent) {
    QSignalBlocker blk(m_volume);
    m_volume->setValue(static_cast<int>(qBound(0.0, percent, 130.0)));
}

void ControlBar::setMuted(bool muted) {
    m_muted = muted;
    m_btnMute->setText(muted ? QStringLiteral("🔇") : QStringLiteral("🔊"));
}

void ControlBar::setSubtitleMenu(QMenu* m) { if (m_btnSubs) m_btnSubs->setMenu(m); }
void ControlBar::setVrMenu(QMenu* m)       { if (m_btnVr)   m_btnVr->setMenu(m); }

void ControlBar::setResumeRemember(bool on) {
    if (!m_btnResume) return;
    QSignalBlocker b(m_btnResume);
    m_btnResume->setChecked(on);
}

bool ControlBar::isResumeRemember() const noexcept {
    return m_btnResume && m_btnResume->isChecked();
}

void ControlBar::setMediaInfo(const QString& text) {
    if (m_lblMediaInfo)
        m_lblMediaInfo->setText(text);
}

void ControlBar::setSpeed(double s) {
    for (int i = 0; i < m_speedBox->count(); ++i) {
        if (qFuzzyCompare(m_speedBox->itemData(i).toDouble(), s)) {
            QSignalBlocker blk(m_speedBox);
            m_speedBox->setCurrentIndex(i);
            return;
        }
    }
}

bool ControlBar::eventFilter(QObject* obj, QEvent* ev) {
    if (obj == m_volume && ev->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->button() == Qt::LeftButton) {
            // Map the click x-position to the slider's value range.
            QStyleOptionSlider opt;
            opt.initFrom(m_volume);
            opt.minimum  = m_volume->minimum();
            opt.maximum  = m_volume->maximum();
            opt.sliderPosition = m_volume->value();
            opt.orientation = m_volume->orientation();

            const QRect groove = m_volume->style()->subControlRect(
                QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, m_volume);
            const QRect handle = m_volume->style()->subControlRect(
                QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, m_volume);

            const int span = groove.width() - handle.width();
            const int pos  = me->pos().x() - groove.x() - handle.width() / 2;
            const int val  = QStyle::sliderValueFromPosition(
                m_volume->minimum(), m_volume->maximum(),
                pos, span);
            m_volume->setValue(val);
            return true;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

} // namespace promp
