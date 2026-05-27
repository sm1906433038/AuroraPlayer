#include "MainWindow.h"

#include "core/MpvPlayer.h"
#include "ui/VideoWidget.h"
#include "ui/ControlBar.h"
#include "ui/SeekBar.h"
#include "ui/TimelineStrip.h"
#include "ui/PlaylistPanel.h"
#include "ui/ThumbnailWorker.h"
#include "ui/QualityAdvancedDialog.h"
#include "vr/VrController.h"
#include "ai/TranscriptionDialog.h"
#include "ai/TranslationDialog.h"
#include "ai/SubtitleExport.h"

#include <QActionGroup>
#include <QApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QGuiApplication>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QHBoxLayout>
#include <QShortcut>
#include <QScreen>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

namespace promp {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("晨曦影音"));
    resize(1280, 760);
    setAcceptDrops(true);
    setMinimumSize(640, 360);

    applyDarkPalette();

    // Match letterboxing: any layout resize gap during pseudo-fullscreen is black
    // instead of flashing the desktop / grey chrome.
    setAutoFillBackground(true);
    {
        QPalette pm = palette();
        pm.setColor(QPalette::Window, Qt::black);
        setPalette(pm);
    }

    m_player   = new MpvPlayer(this);
    m_vr       = new VrController(this);

    buildUi();
    buildMenus();
    wireSignals();
    wireShortcuts();

    m_thumbs = new ThumbnailWorker(this);
    // Deliberately do NOT m_thumbs->start() here — see onMediaLoaded().

    // SeekBar no longer requests thumbnails — it only shows a live time
    // tooltip. ThumbnailWorker stays alive solely for the timeline strip.
    m_strip->setWorker(m_thumbs);
    connect(m_thumbs, &ThumbnailWorker::thumbnailReady,
            m_strip,  &TimelineStrip::onThumbnailReady,    Qt::QueuedConnection);
    connect(m_player, &MpvPlayer::durationChanged,
            m_strip,  &TimelineStrip::setDuration);
    connect(m_player, &MpvPlayer::positionChanged,
            m_strip,  &TimelineStrip::setPosition);
    connect(m_strip,  &TimelineStrip::seekRequested, this,
            [this](double s) { m_player->seekAbsolute(s, true); });

    // Playlist routing.
    connect(m_playlist, &PlaylistPanel::playRequested,
            this,       &MainWindow::openPath);
    connect(m_player, &MpvPlayer::endFile, this, [this](int reason) {
        persistResumePosition();
        if (m_resumeTimer) m_resumeTimer->stop();
        // mpv reason: 0=EOF, 1=stop, 2=quit, 3=error. Auto-advance only on EOF.
        if (reason == 0 && m_playlist) m_playlist->playNext();
    });

    // Fullscreen auto-hide timer.
    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(true);
    m_idleTimer->setInterval(2000);
    connect(m_idleTimer, &QTimer::timeout, this, [this]() {
        if (!m_fsActive) return;
        m_controls->hide();
        statusBar()->hide();
        QApplication::setOverrideCursor(Qt::BlankCursor);
        m_overlayHidden = true;
    });

    m_resumeTimer = new QTimer(this);
    m_resumeTimer->setInterval(2500);
    connect(m_resumeTimer, &QTimer::timeout, this, &MainWindow::persistResumePosition);

    // We track mouse movement on the whole window for the idle timer.
    qApp->installEventFilter(this);

    loadSettings();

    statusBar()->showMessage(tr("就绪 — 把视频拖进窗口，或按 Ctrl+O 打开"), 0);
}

MainWindow::~MainWindow() {
    saveSettings();
    if (m_thumbs) {
        m_thumbs->stop();
        delete m_thumbs;     // parent-less, so we must delete it ourselves
        m_thumbs = nullptr;
    }
}

void MainWindow::applyDarkPalette() {
    QPalette p = palette();
    p.setColor(QPalette::Window,        QColor(20, 20, 22));
    p.setColor(QPalette::WindowText,    QColor(230, 230, 230));
    p.setColor(QPalette::Base,          QColor(28, 28, 30));
    p.setColor(QPalette::AlternateBase, QColor(36, 36, 38));
    p.setColor(QPalette::Text,          QColor(230, 230, 230));
    p.setColor(QPalette::Button,        QColor(40, 40, 44));
    p.setColor(QPalette::ButtonText,    QColor(230, 230, 230));
    p.setColor(QPalette::Highlight,     QColor(64, 132, 220));
    p.setColor(QPalette::HighlightedText, Qt::white);
    qApp->setPalette(p);
    qApp->setStyleSheet(R"(
        QMenuBar { background:#181819; color:#e6e6e6; }
        QMenuBar::item:selected { background:#303034; }
        QMenu { background:#222226; color:#e6e6e6; border:1px solid #333; }
        QMenu::item:selected { background:#3a3a40; }
        QStatusBar { background:#16161a; color:#9b9ba2; }
        #ControlBar { background:#16161a; }
        QSlider::groove:horizontal {
            height: 6px; background: #2a2a2e; border-radius: 3px;
        }
        QSlider::sub-page:horizontal { background:#4084dc; border-radius:3px; }
        QSlider::handle:horizontal {
            width:14px; margin:-5px 0; background:#dfe2e6;
            border-radius:7px;
        }
    )");
}

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    central->setAutoFillBackground(true);
    {
        QPalette pal = central->palette();
        pal.setColor(QPalette::Window, Qt::black);
        central->setPalette(pal);
    }

    // [content area .................][sidebar tab][dock]
    auto* hlay = new QHBoxLayout(central);
    hlay->setContentsMargins(0, 0, 0, 0);
    hlay->setSpacing(0);

    auto* contentArea = new QWidget(central);
    auto* vlay        = new QVBoxLayout(contentArea);
    vlay->setContentsMargins(0, 0, 0, 0);
    vlay->setSpacing(0);

    m_video = new VideoWidget(contentArea);
    m_video->attachPlayer(m_player);
    m_video->attachVrController(m_vr);

    m_strip = new TimelineStrip(contentArea);
    m_strip->setVisible(false);   // off by default; toggle via View menu.

    m_controls = new ControlBar(contentArea);

    vlay->addWidget(m_video, /*stretch*/ 1);
    vlay->addWidget(m_strip, 0);
    vlay->addWidget(m_controls, 0);

    m_contentArea  = contentArea;
    m_contentVLayout = vlay;
    // Vertical "播放列表" tab — a small label-sized button anchored to the
    // right edge of the content area, vertically centred. Click to open or
    // close the dockable playlist panel.
    m_btnSidebar = new QToolButton(central);
    m_btnSidebar->setCheckable(true);
    m_btnSidebar->setCursor(Qt::PointingHandCursor);
    m_btnSidebar->setText(QStringLiteral("播\n放\n列\n表"));
    m_btnSidebar->setToolTip(tr("显示 / 隐藏播放列表 (Ctrl+L)"));
    m_btnSidebar->setFixedSize(20, 96);
    m_btnSidebar->setStyleSheet(
        "QToolButton {"
        "  background:#1a1a1f; color:#b6b9bd; border:none;"
        "  border-top-left-radius:6px; border-bottom-left-radius:6px;"
        "  padding: 6px 0; font: 11px 'Microsoft YaHei UI';"
        "}"
        "QToolButton:hover    { background:#2a2a32; color:#ffffff; }"
        "QToolButton:checked  { background:#1e6cd6; color:#ffffff; }");

    // Wrap the tab button in a thin column with stretches above + below so
    // it floats centred against the right edge without occupying full height.
    auto* sideCol = new QWidget(central);
    sideCol->setFixedWidth(20);
    sideCol->setAttribute(Qt::WA_TranslucentBackground);
    auto* sideLay = new QVBoxLayout(sideCol);
    sideLay->setContentsMargins(0, 0, 0, 0);
    sideLay->setSpacing(0);
    sideLay->addStretch(1);
    sideLay->addWidget(m_btnSidebar);
    sideLay->addStretch(1);

    hlay->addWidget(contentArea, /*stretch*/ 1);
    hlay->addWidget(sideCol, 0);

    setCentralWidget(central);

    // Dockable playlist panel.
    m_playlist = new PlaylistPanel(this);
    addDockWidget(Qt::RightDockWidgetArea, m_playlist);
    m_playlist->setVisible(false);

    // Two-way binding between the tab button and the dock visibility.
    connect(m_btnSidebar, &QToolButton::toggled, this, [this](bool on) {
        if (m_playlist && m_playlist->isVisible() != on)
            m_playlist->setVisible(on);
    });
    connect(m_playlist, &QDockWidget::visibilityChanged,
            m_btnSidebar, &QToolButton::setChecked);
}

void MainWindow::buildMenus() {
    auto* mFile = menuBar()->addMenu(tr("文件(&F)"));
    auto* aOpen   = mFile->addAction(tr("打开文件(&O)..."), QKeySequence::Open, this, &MainWindow::onOpenFile);
    auto* aOpenUrl= mFile->addAction(tr("打开网址(&U)..."), QKeySequence("Ctrl+U"), this, &MainWindow::onOpenUrl);
    m_menuRecent = mFile->addMenu(tr("最近播放(&R)"));
    rebuildRecentMenu();
    mFile->addSeparator();
    auto* aShot   = mFile->addAction(tr("截图(&S)"), QKeySequence("Ctrl+S"), this, &MainWindow::onScreenshot);
    mFile->addSeparator();
    mFile->addAction(tr("退出(&X)"), QKeySequence::Quit, this, &QWidget::close);
    Q_UNUSED(aOpen); Q_UNUSED(aOpenUrl); Q_UNUSED(aShot);

    auto* mPlay = menuBar()->addMenu(tr("播放(&P)"));
    mPlay->addAction(tr("播放 / 暂停"), QKeySequence(Qt::Key_Space), m_player, &MpvPlayer::togglePause);
    // 句点 . 在 wireShortcuts() 里被分配给「逐帧前进」，所以「停止」用 Ctrl+. 触发。
    mPlay->addAction(tr("停止"), QKeySequence("Ctrl+."), this, [this]() {
        persistResumePosition();
        m_player->stop();
    });
    mPlay->addSeparator();
    mPlay->addAction(tr("后退 5 秒"), QKeySequence(Qt::Key_Left),
                     this, [this]() { m_player->seekRelative(-5); });
    mPlay->addAction(tr("前进 5 秒"), QKeySequence(Qt::Key_Right),
                     this, [this]() { m_player->seekRelative(+5); });
    mPlay->addAction(tr("后退 1 分钟"), QKeySequence(Qt::Key_Down),
                     this, [this]() { m_player->seekRelative(-60); });
    mPlay->addAction(tr("前进 1 分钟"), QKeySequence(Qt::Key_Up),
                     this, [this]() { m_player->seekRelative(+60); });

    auto* mAudio = menuBar()->addMenu(tr("音轨(&A)"));
    m_menuAudio = mAudio;
    rebuildTrackMenus();

    auto* mSubs = menuBar()->addMenu(tr("字幕(&S)"));
    m_menuSubs = mSubs;
    rebuildTrackMenus();

    auto* mView = menuBar()->addMenu(tr("视图(&V)"));
    mView->addAction(tr("全屏(&F)"), QKeySequence(Qt::Key_F),
                     this, &MainWindow::onToggleFullscreen);
    mView->addSeparator();

    m_actToggleStrip = mView->addAction(tr("显示缩略图条"), QKeySequence("Ctrl+T"));
    m_actToggleStrip->setCheckable(true);
    connect(m_actToggleStrip, &QAction::toggled, m_strip, &QWidget::setVisible);

    m_actTogglePlaylist = mView->addAction(tr("显示播放列表"), QKeySequence("Ctrl+L"));
    m_actTogglePlaylist->setCheckable(true);
    connect(m_actTogglePlaylist, &QAction::toggled, m_playlist, &QWidget::setVisible);
    // Reflect "X" on the dock title bar back into the menu state.
    connect(m_playlist, &QDockWidget::visibilityChanged,
            m_actTogglePlaylist, &QAction::setChecked);

    // ---- 画质 -------------------------------------------------------------
    auto* mQual = menuBar()->addMenu(tr("画质(&Q)"));
    auto* grpQual = new QActionGroup(this);
    grpQual->setExclusive(true);
    m_actQualNative   = mQual->addAction(tr("原生（无任何增强，直出）"));
    m_actQualStandard = mQual->addAction(tr("标准（兼容）"));
    m_actQualHigh     = mQual->addAction(tr("高画质（推荐）"));
    m_actQualUltra    = mQual->addAction(tr("极致（运动插帧）"));
    for (auto* a : { m_actQualNative, m_actQualStandard, m_actQualHigh, m_actQualUltra }) {
        a->setCheckable(true);
        grpQual->addAction(a);
    }
    m_actQualHigh->setChecked(true);   // sensible default; loadSettings() overrides

    mQual->addSeparator();
    auto* aQualHelp = mQual->addAction(tr("各档位说明…"));
    connect(aQualHelp, &QAction::triggered, this, [this]() {
        QMessageBox::information(this, tr("画质档位"),
            tr("<b>原生</b>：bilinear、tone-mapping=clip、gamut-mapping=clip、"
               "去色带 / 插帧 / 抖动 全部关闭。<b>不做任何处理</b>，"
               "解码出来什么就显示什么——纯粹主义者的选择。<br>"
               "<b>标准</b>：gpu-dumb-mode=yes，关闭所有 advanced shader 路径，"
               "但保留合理的色调映射（bt.2446a）以正确显示 HDR↔SDR。<br>"
               "<b>高画质</b>：ewa_lanczossharp 上采样、spline36 色度、去色带、"
               "动态 HDR 峰值、rgba16f 16-bit 中间缓冲。<br>"
               "<b>极致</b>：在高画质基础上启用 24p → 高刷运动插帧"
               "（tscale=oversample），配合 video-sync=display-resample。<br><br>"
               "<b>注意</b>：原生/标准 ↔ 高/极致 互切会重新加载当前文件以重建 GPU 渲染管线"
               "（gpu-dumb-mode 与 fbo-format 是 pre-init 属性）；进度自动保留。<br><br>"
               "<b>测试要点</b>：<br>"
               "• 插帧差别只在 <b>24p 电影 + 高刷屏</b>上明显（看快摇镜头）。<br>"
               "• 去色带差别只在 <b>有色带的 SDR 片源</b>（天空、暗场渐变）。<br>"
               "• HDR 峰值仅在 <b>HDR 片源</b>。<br>"
               "• 高质上采样仅在 <b>视频分辨率 &lt; 屏幕分辨率</b>（如 1080p 看 4K 屏）。<br>"
               "4K 高码率 SDR 原片 + 4K 屏直出本身已经无损，档位差别会很小。<br><br>"
               "<b>原生模式只关掉渲染管线的增强</b>。如果你还想关掉色彩/锐化/降噪/着色器，"
               "请去画质调节里点「恢复默认」、并到「自定义着色器」里禁用全部。"));
    });

    auto* aQualInspect = mQual->addAction(tr("显示当前画质参数…"));
    connect(aQualInspect, &QAction::triggered, this, [this]() {
        const QStringList keys = {
            QStringLiteral("gpu-dumb-mode"), QStringLiteral("scale"),
            QStringLiteral("cscale"),        QStringLiteral("dscale"),
            QStringLiteral("dither-depth"),  QStringLiteral("correct-downscaling"),
            QStringLiteral("linear-downscaling"), QStringLiteral("sigmoid-upscaling"),
            QStringLiteral("deband"),        QStringLiteral("deband-iterations"),
            QStringLiteral("deband-threshold"),
            QStringLiteral("hdr-compute-peak"), QStringLiteral("tone-mapping"),
            QStringLiteral("fbo-format"),    QStringLiteral("interpolation"),
            QStringLiteral("tscale"),        QStringLiteral("video-sync"),
            QStringLiteral("hwdec-current"),
            QStringLiteral("display-fps-override"),
            QStringLiteral("estimated-display-fps"),
            QStringLiteral("container-fps"),
            QStringLiteral("estimated-vf-fps"),
            QStringLiteral("video-out-params/pixelformat"),
            QStringLiteral("video-params/sig-peak"),
        };
        QString html = QStringLiteral("<table cellpadding='3' style='font-family:Consolas,monospace;'>");
        for (const auto& k : keys) {
            const QVariant v = m_player->getProperty(k.toUtf8());
            html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>")
                        .arg(k.toHtmlEscaped(),
                             v.isValid() ? v.toString().toHtmlEscaped()
                                         : QStringLiteral("<i>(unset)</i>"));
        }
        html += QStringLiteral("</table>");
        QMessageBox box(QMessageBox::Information, tr("mpv 当前画质参数"),
                        tr("以下为 mpv 实际生效的值（用于验证档位是否真正应用）："),
                        QMessageBox::Ok, this);
        box.setInformativeText(html);
        box.setTextFormat(Qt::RichText);
        box.exec();
    });

    connect(m_actQualNative, &QAction::triggered, this, [this]() {
        m_player->setQualityPreset(MpvPlayer::QualityPreset::Native);
        m_player->showText(tr("画质：原生（直出）"), 1500);
    });
    connect(m_actQualStandard, &QAction::triggered, this, [this]() {
        m_player->setQualityPreset(MpvPlayer::QualityPreset::Standard);
        m_player->showText(tr("画质：标准"), 1500);
    });
    connect(m_actQualHigh, &QAction::triggered, this, [this]() {
        m_player->setQualityPreset(MpvPlayer::QualityPreset::High);
        m_player->showText(tr("画质：高画质"), 1500);
    });
    connect(m_actQualUltra, &QAction::triggered, this, [this]() {
        m_player->setQualityPreset(MpvPlayer::QualityPreset::Ultra);
        m_player->showText(tr("画质：极致"), 1500);
    });

    // ---- 硬件加速 ----
    mQual->addSeparator();
    auto* mHw = mQual->addMenu(tr("硬件加速"));
    auto* grpHw = new QActionGroup(this);
    grpHw->setExclusive(true);
    m_actHwOff      = mHw->addAction(tr("软解（CPU）— 兼容性最高"));
    m_actHwCopy     = mHw->addAction(tr("硬解 复制模式 — 1080p / 4K 稳定首选"));
    m_actHwZeroCopy = mHw->addAction(tr("硬解 零复制 — 8K / VR / 高帧率 推荐 ⚡"));
    for (auto* a : { m_actHwOff, m_actHwCopy, m_actHwZeroCopy }) {
        a->setCheckable(true);
        grpHw->addAction(a);
    }
    connect(m_actHwOff, &QAction::triggered, this, [this]() {
        m_player->setHwAccel(MpvPlayer::HwAccel::Off);
        m_player->showText(tr("硬件加速：软解（CPU）"), 1500);
    });
    connect(m_actHwCopy, &QAction::triggered, this, [this]() {
        m_player->setHwAccel(MpvPlayer::HwAccel::CopyMode);
        m_player->showText(tr("硬件加速：复制模式"), 1500);
    });
    connect(m_actHwZeroCopy, &QAction::triggered, this, [this]() {
        m_player->setHwAccel(MpvPlayer::HwAccel::ZeroCopy);
        m_player->showText(tr("硬件加速：零复制 ⚡"), 1500);
    });
    mHw->addSeparator();
    auto* aHwHelp = mHw->addAction(tr("各模式说明 / 性能对比…"));
    connect(aHwHelp, &QAction::triggered, this, [this]() {
        QMessageBox::information(this, tr("硬件加速模式"),
            tr("<b>软解（CPU）</b>：完全用 CPU 解码。最大兼容性，"
               "但 4K 以上 CPU 占用极高，8K 基本播不动。<br><br>"
               "<b>硬解 复制模式</b>（之前的默认）：GPU 解码 → 把解码后的帧 <b>复制回内存</b> "
               "→ 再上传到 GL 渲染器。每帧多走一次 PCIe，4K@60 之内绰绰有余；"
               "<b>8K@60 每帧 ~100MB 来回拷贝</b>，PCIe 4.0×16 也会拖出明显卡顿。<br><br>"
               "<b>硬解 零复制</b> ⚡：GPU 解码后通过 D3D11VA-OpenGL interop "
               "<b>不经过 CPU 内存</b>直接喂给 mpv 渲染器。对你的 RTX 4080 + 8K/VR "
               "是<b>正确选择</b>；性能可提升 30~50%，VRAM 占用略增。<br><br>"
               "切换需要重新加载当前文件以重建解码管线，会自动保留进度。<br><br>"
               "<b>关于降噪</b>：hqdn3d 是 <b>CPU 滤镜</b>，跟硬解无关。"
               "8K 像素量 4 倍于 4K，CPU 跑这个邻域滤波必卡。"
               "高分辨率请关闭降噪，或换用 GPU 着色器降噪（自定义着色器目录）。"));
    });

    // ---- Advanced quality: HDR + picture adjust + shaders ----
    mQual->addSeparator();
    auto* aHdrAdj = mQual->addAction(tr("HDR / 色调映射…"));
    connect(aHdrAdj, &QAction::triggered, this, [this]() {
        QualityAdvancedDialog dlg(m_player, this);
        dlg.exec();   // dlg applies live & persists on OK / reverts on Cancel
    });
    auto* aPicAdj = mQual->addAction(tr("画质调节（色彩 / 锐化 / 降噪）…"));
    connect(aPicAdj, &QAction::triggered, this, [this]() {
        QualityAdvancedDialog dlg(m_player, this);
        dlg.selectTab(1);
        dlg.exec();
    });

    // Shaders submenu — populated dynamically from the user shader folder.
    m_menuShaders = mQual->addMenu(tr("自定义着色器"));
    connect(m_menuShaders, &QMenu::aboutToShow, this, &MainWindow::rebuildShaderMenu);

    auto* mVr   = menuBar()->addMenu(tr("VR / 360°"));
    m_menuVr = mVr;
    auto* grpProj = new QActionGroup(this);
    grpProj->setExclusive(true);
    m_actVrOff = mVr->addAction(tr("关闭"));                m_actVrOff->setCheckable(true); m_actVrOff->setChecked(true);
    m_actVr360 = mVr->addAction(tr("360° 全景（等距投影）")); m_actVr360->setCheckable(true);
    m_actVr180 = mVr->addAction(tr("180° 全景（等距投影）")); m_actVr180->setCheckable(true);
    grpProj->addAction(m_actVrOff); grpProj->addAction(m_actVr360); grpProj->addAction(m_actVr180);

    mVr->addSeparator();
    auto* grpStereo = new QActionGroup(this);
    grpStereo->setExclusive(true);
    m_actStereoMono = mVr->addAction(tr("单视（非 3D）"));        m_actStereoMono->setCheckable(true); m_actStereoMono->setChecked(true);
    m_actStereoSBS  = mVr->addAction(tr("3D 左右排列（SBS）"));   m_actStereoSBS->setCheckable(true);
    m_actStereoTB   = mVr->addAction(tr("3D 上下排列（TB）"));    m_actStereoTB->setCheckable(true);
    grpStereo->addAction(m_actStereoMono);
    grpStereo->addAction(m_actStereoSBS);
    grpStereo->addAction(m_actStereoTB);

    mVr->addSeparator();
    mVr->addAction(tr("重置视角"), QKeySequence(Qt::Key_Home),
                   this, [this]() { m_vr->resetView(); });

    // ---- AI 字幕（whisper.cpp 本地转写） --------------------------------
    auto* mAi = menuBar()->addMenu(tr("AI 字幕(&I)"));
    auto* aAiGen = mAi->addAction(tr("生成字幕…"));
    connect(aAiGen, &QAction::triggered, this, &MainWindow::onAiGenerateSubtitles);
    auto* aAiTrans = mAi->addAction(tr("翻译字幕…"));
    connect(aAiTrans, &QAction::triggered, this, &MainWindow::onAiTranslateSubtitles);
    mAi->addSeparator();
    auto* aAiAbout = mAi->addAction(tr("关于 AI 字幕 …"));
    connect(aAiAbout, &QAction::triggered, this, [this]() {
        QMessageBox::information(this, tr("关于 AI 字幕"),
            tr("基于 <b>whisper.cpp</b> 的本地语音识别。<br>"
               "首次使用需下载模型（约 1 GB）。RTX 4080 等卡可自动启用 CUDA，"
               "大约可达实时速度的 5–20×。<br><br>"
               "默认模型：<code>large-v3 量化 (q5_0)</code>，约 1.08 GB，通用多语言。<br>"
               "日 → 中 专用：<code>海南鸡 v2 5000h</code> 微调（与 TransWithAI 同源）。<br><br>"
               "模型目录：<code>%1</code>")
              .arg(QDir::toNativeSeparators(
                   QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                   + QStringLiteral("/models"))));
    });

    auto* mHelp = menuBar()->addMenu(tr("帮助(&H)"));
    mHelp->addAction(tr("关于 晨曦影音(&A)"), this, &MainWindow::onAbout);

    // Hook the in-control-bar shortcut buttons to the just-built menus.
    if (m_controls) {
        m_controls->setSubtitleMenu(m_menuSubs);
        m_controls->setVrMenu(m_menuVr);
    }
}

void MainWindow::wireSignals() {
    // mpv -> UI
    connect(m_player, &MpvPlayer::durationChanged, m_controls, &ControlBar::setDuration);
    connect(m_player, &MpvPlayer::durationChanged, this,        &MainWindow::tryApplyResume);
    connect(m_player, &MpvPlayer::positionChanged, m_controls, &ControlBar::setPosition);
    connect(m_player, &MpvPlayer::pausedChanged,   m_controls, &ControlBar::setPaused);
    connect(m_player, &MpvPlayer::volumeChanged,   m_controls, &ControlBar::setVolume);
    connect(m_player, &MpvPlayer::muteChanged,     m_controls, &ControlBar::setMuted);
    connect(m_player, &MpvPlayer::speedChanged,    m_controls, &ControlBar::setSpeed);
    connect(m_player, &MpvPlayer::mediaTitleChanged, this,
            [this](const QString& t) {
                setWindowTitle(t.isEmpty() ? QStringLiteral("晨曦影音")
                                           : t + QStringLiteral("  —  晨曦影音"));
            });
    connect(m_player, &MpvPlayer::logMessage, this, &MainWindow::onMpvLog);
    connect(m_player, &MpvPlayer::tracksChanged, this, &MainWindow::onTracksChanged);
    connect(m_player, &MpvPlayer::volumeChanged, this, &MainWindow::onMpvVolume);
    connect(m_player, &MpvPlayer::muteChanged,   this, &MainWindow::onMpvMute);

    // UI -> mpv
    connect(m_controls, &ControlBar::playPauseClicked, m_player, &MpvPlayer::togglePause);
    connect(m_controls, &ControlBar::prevClicked, this, [this]{ if (m_playlist) m_playlist->playPrevious(); });
    connect(m_controls, &ControlBar::nextClicked, this, [this]{ if (m_playlist) m_playlist->playNext(); });
    // Keyframe navigation: mpv's `seek <amount> keyframes` snaps to the nearest
    // keyframe AFTER seeking `amount` seconds. A small positive/negative amount
    // therefore jumps to the adjacent keyframe in that direction.
    connect(m_controls, &ControlBar::prevKeyframeClicked, this,
            [this]{ m_player->sendCommand({ "seek", "-5", "keyframes" }); });
    connect(m_controls, &ControlBar::nextKeyframeClicked, this,
            [this]{ m_player->sendCommand({ "seek", "5",  "keyframes" }); });
    connect(m_controls, &ControlBar::volumeChanged,    m_player, &MpvPlayer::setVolume);
    connect(m_controls, &ControlBar::muteToggled,      this,
            [this]() { m_player->setMute(!m_player->getProperty("mute").toBool()); });
    connect(m_controls, &ControlBar::speedChanged,     m_player, &MpvPlayer::setSpeed);
    connect(m_controls, &ControlBar::fullscreenToggled,this, &MainWindow::onToggleFullscreen);

    connect(m_controls, &ControlBar::resumeRememberToggled, this, [this](bool on) {
        QSettings st;
        st.setValue(QStringLiteral("player/resumeRemember"), on);
        if (on && m_resumeTimer && !m_resumeMediaKey.isEmpty())
            m_resumeTimer->start();
        else if (m_resumeTimer)
            m_resumeTimer->stop();
    });

    connect(m_controls->seekBar(), &SeekBar::seekRequested, this,
            [this](double seconds, bool exact) { m_player->seekAbsolute(seconds, exact); });

    connect(m_controls->seekBar(), &SeekBar::seekScrubBegan, this, [this]() {
        m_wasPausedBeforeSeekScrub = m_player->getProperty("pause").toBool();
        m_player->pause();
    }, Qt::DirectConnection);
    connect(m_controls->seekBar(), &SeekBar::seekScrubEnded, this, [this]() {
        if (!m_wasPausedBeforeSeekScrub)
            m_player->play();
    }, Qt::DirectConnection);

    // Video widget interactions
    connect(m_video, &VideoWidget::doubleClicked, this, &MainWindow::onToggleFullscreen);
    connect(m_video, &VideoWidget::videoClicked,   m_player, &MpvPlayer::togglePause);
    connect(m_video, &VideoWidget::rightClicked, this, [this](const QPoint& globalPos) {
        auto normRot = [](int r) {
            r %= 360;
            if (r < 0) r += 360;
            return r;
        };
        QMenu menu(this);
        menu.addAction(tr("顺时针旋转 90°"), this, [this, normRot] {
            const int r = qRound(m_player->getProperty("video-rotate").toDouble());
            // mpv's video-rotate is INT64 — MpvPlayer passes DOUBLE as MPV_FORMAT_DOUBLE;
            // wrong format makes mpv_set_property fail (silently in our wrapper).
            m_player->setProperty("video-rotate", normRot(r + 90));
        });
        menu.addAction(tr("逆时针旋转 90°"), this, [this, normRot] {
            const int r = qRound(m_player->getProperty("video-rotate").toDouble());
            m_player->setProperty("video-rotate", normRot(r - 90));
        });
        menu.exec(globalPos);
    });
    connect(m_video, &VideoWidget::wheelScrolled, this, [this](int dy) {
        const double cur = m_player->getProperty("volume").toDouble();
        m_player->setVolume(qBound(0.0, cur + (dy > 0 ? 5.0 : -5.0), 130.0));
    });

    // VR menu wiring
    connect(m_actVrOff, &QAction::triggered, this,
            [this]() { m_vr->setProjection(VrController::Projection::None); });
    connect(m_actVr360, &QAction::triggered, this,
            [this]() { m_vr->setProjection(VrController::Projection::Equirect360); });
    connect(m_actVr180, &QAction::triggered, this,
            [this]() { m_vr->setProjection(VrController::Projection::Equirect180); });
    connect(m_actStereoMono,&QAction::triggered, this,
            [this]() { m_vr->setStereo(VrController::Stereo::Mono); });
    connect(m_actStereoSBS, &QAction::triggered, this,
            [this]() { m_vr->setStereo(VrController::Stereo::SBS); });
    connect(m_actStereoTB,  &QAction::triggered, this,
            [this]() { m_vr->setStereo(VrController::Stereo::TB); });

    connect(m_player, &MpvPlayer::fileLoaded, this, &MainWindow::onMediaLoaded);
}

void MainWindow::startThumbnailWorkerIfNeeded() {
    if (!m_thumbs || m_thumbWorkerStarted) return;
    m_thumbWorkerStarted = true;
    m_thumbs->start();
}

void MainWindow::onMediaLoaded(const QString& path) {
    startThumbnailWorkerIfNeeded();
    if (m_thumbs) m_thumbs->setSource(path);
    const QString k = mediaKeyForResume(path);
    if (!k.isEmpty())
        m_resumeMediaKey = k;
    if (m_controls && m_controls->isResumeRemember() && m_resumeTimer)
        m_resumeTimer->start();

    if (m_controls) {
        const int w = m_player->getProperty("width").toInt();
        const int h = m_player->getProperty("height").toInt();
        const double fileSize = m_player->getProperty("file-size").toDouble();
        const double duration = m_player->getProperty("duration").toDouble();

        QStringList parts;
        if (w > 0 && h > 0)
            parts << QStringLiteral("%1×%2").arg(w).arg(h);
        if (fileSize > 0 && duration > 0) {
            const double kbps = fileSize * 8.0 / (duration * 1000.0);
            if (kbps >= 1000)
                parts << QStringLiteral("%1 Mbps").arg(kbps / 1000.0, 0, 'f', 1);
            else
                parts << QStringLiteral("%1 kbps").arg(kbps, 0, 'f', 0);
        }
        m_controls->setMediaInfo(parts.join(QStringLiteral("  ")));
    }
}

// -----------------------------------------------------------------------------

void MainWindow::openPath(const QString& path) {
    if (path.isEmpty()) return;
    m_resumeMediaKey         = mediaKeyForResume(path);
    m_resumeAppliedForFile   = false;
    m_player->loadFile(path);
    statusBar()->showMessage(tr("正在加载：%1").arg(path), 3000);
    addRecent(path);
    QFileInfo fi(path);
    if (fi.exists() && fi.isFile()) m_lastOpenDir = fi.absolutePath();
    if (m_playlist) {
        if (fi.exists() && fi.isFile())
            syncPlaylistWithSameFolder(path);
        else
            m_playlist->notePlaying(path);
    }
    if (m_strip) m_strip->setSource(path);
}

void MainWindow::onOpenFile() {
    static const QString kFilter =
        tr("视频文件 (*.mp4 *.mkv *.webm *.mov *.avi *.flv *.ts *.m2ts *.wmv *.vob);;"
           "音频文件 (*.mp3 *.flac *.aac *.ogg *.opus *.wav *.m4a);;"
           "所有文件 (*.*)");
    const QString f = QFileDialog::getOpenFileName(this, tr("打开"), m_lastOpenDir, kFilter);
    if (!f.isEmpty()) openPath(f);
}

void MainWindow::onOpenUrl() {
    const QString url = QInputDialog::getText(this, tr("打开网址"), tr("媒体地址："));
    if (!url.isEmpty()) openPath(url);
}

void MainWindow::layoutFullscreenControlBar() {
    if (!m_fsActive || !m_contentArea || !m_controls) return;
    const int w = m_contentArea->width();
    const int hTotal = m_contentArea->height();
    if (w <= 0 || hTotal <= 0) return;

    m_controls->adjustSize();
    int ch = m_controls->height();
    if (ch <= 0)
        ch = m_controls->sizeHint().height();
    if (ch <= 0)
        ch = 88;

    m_controls->setGeometry(0, hTotal - ch, w, ch);
    m_controls->raise();
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    if (m_fsActive)
        layoutFullscreenControlBar();
}

void MainWindow::bumpVideoRedraw() {
    if (!m_video) return;
    m_video->update();
    QTimer::singleShot(0, m_video, [v = QPointer<VideoWidget>(m_video)] {
        if (v) v->update();
    });
    QTimer::singleShot(48, m_video, [v = QPointer<VideoWidget>(m_video)] {
        if (v) v->update();
    });
}

// Fullscreen without Qt::WindowFullScreen / showFullScreen():
// on Windows, the real OS full-screen path makes DWM inject a black frame.
// We stay in normal Win32 window mode: clear maximized/fullscreen bits, add
// Qt::FramelessWindowHint, and stretch to QScreen::availableGeometry() for the
// screen that contains the window (work area — taskbar stays visible). Using
// full geometry() instead caused a harsher resize + DWM overlap with the
// taskbar region and was linked to visible black frames on toggle.
// HWND / GL context stay live.
void MainWindow::onToggleFullscreen() {
    if (!m_fsActive) {
        m_fsSavedGeo    = geometry();
        m_fsSavedStates = windowState();
        m_fsSavedFlags  = windowFlags();

        menuBar()->hide();
        statusBar()->hide();

        QScreen* scr = QGuiApplication::screenAt(frameGeometry().center());
        if (!scr)
            scr = screen();
        if (!scr)
            scr = QGuiApplication::primaryScreen();

        setWindowState(Qt::WindowNoState);

        setWindowFlags(m_fsSavedFlags | Qt::FramelessWindowHint);

        if (scr) {
            const QRect g = scr->availableGeometry();
            if (g.isValid())
                setGeometry(g);
        }

        show();
        raise();
        activateWindow();

        m_fsActive = true;

        // Overlay the control bar — do not give it a layout row (avoids video resize
        // / zoom when the auto-hide timer shows or hides the bar).
        if (m_contentVLayout && m_controls)
            m_contentVLayout->removeWidget(m_controls);
        m_controls->setParent(m_contentArea);
        m_controls->show();
        layoutFullscreenControlBar();
        m_controls->raise();

        if (m_idleTimer) m_idleTimer->start();

        bumpVideoRedraw();
    } else {
        if (m_overlayHidden) {
            QApplication::restoreOverrideCursor();
            m_overlayHidden = false;
        }
        if (m_idleTimer) m_idleTimer->stop();

        if (m_contentVLayout && m_controls)
            m_contentVLayout->addWidget(m_controls);

        setWindowFlags(m_fsSavedFlags);

        if (!m_fsSavedGeo.isNull())
            setGeometry(m_fsSavedGeo);
        setWindowState(m_fsSavedStates);

        menuBar()->show();
        statusBar()->show();
        m_controls->show();

        show();
        raise();

        m_fsActive = false;

        bumpVideoRedraw();
    }
}

void MainWindow::changeEvent(QEvent* e) {
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::WindowStateChange)
        bumpVideoRedraw();
}

void MainWindow::wakeCursor() {
    if (!m_fsActive) return;
    if (m_overlayHidden) {
        QApplication::restoreOverrideCursor();
        m_overlayHidden = false;
        m_controls->show();
        // Do not show QStatusBar here: it steals height from the central widget
        // and would resize the video even though the control bar is overlaid.
        layoutFullscreenControlBar();
        m_controls->raise();
    }
    if (m_idleTimer) m_idleTimer->start();
}

bool MainWindow::eventFilter(QObject* obj, QEvent* ev) {
    if (m_fsActive) {
        switch (ev->type()) {
            case QEvent::MouseMove:
            case QEvent::MouseButtonPress:
            case QEvent::Wheel:
            case QEvent::KeyPress:
                wakeCursor();
                break;
            default:
                break;
        }
    }
    return QMainWindow::eventFilter(obj, ev);
}

void MainWindow::onScreenshot() {
    m_player->screenshot();
    statusBar()->showMessage(tr("截图已保存"), 2500);
}

void MainWindow::onAbout() {
    QMessageBox::about(this, tr("关于 晨曦影音"),
        tr("<h3>晨曦影音 <span style='color:gray;font-size:small;'>(AuroraPlayer)</span></h3>"
           "<p>基于 <b>libmpv</b> + <b>Qt 6</b> + <b>C++20</b> 构建的高性能播放器。</p>"
           "<ul>"
           "<li>支持 VP9 / AV1 / HEVC，最高 8K 分辨率</li>"
           "<li>NVDEC / D3D11VA 硬件解码 + HDR 色调映射</li>"
           "<li>真 360° / 180° 球面投影 + 立体 3D（SBS / TB）</li>"
           "<li>进度条悬浮缩略图预览</li>"
           "</ul>"
           "<p><b>项目地址：</b><a href=\"https://github.com/sm1906433038/AuroraPlayer\">"
           "https://github.com/sm1906433038/AuroraPlayer</a></p>"));
}

void MainWindow::onMpvLog(const QString& level, const QString& component, const QString& text) {
    if (level == "error" || level == "fatal") {
        statusBar()->showMessage(QString("[%1] %2: %3").arg(level, component, text), 5000);
    }
}

// ---------- events -----------------------------------------------------------

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
    const auto urls = e->mimeData()->urls();
    if (urls.isEmpty()) return;

    QStringList paths;
    for (const QUrl& u : urls) {
        paths << (u.isLocalFile() ? u.toLocalFile() : u.toString());
    }
    if (paths.isEmpty()) return;

    // First file plays immediately; subsequent ones go to the playlist queue
    // (whether or not the dock is visible — items remain queued).
    openPath(paths.first());
    if (m_playlist && paths.size() > 1) {
        m_playlist->addPaths(paths.mid(1));
    }
}

void MainWindow::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
        case Qt::Key_Escape:
            if (m_fsActive) onToggleFullscreen();
            else QMainWindow::keyPressEvent(e);
            return;
        case Qt::Key_M:
            m_player->setMute(!m_player->getProperty("mute").toBool());
            return;
        default:
            QMainWindow::keyPressEvent(e);
    }
}

void MainWindow::closeEvent(QCloseEvent* e) {
    saveSettings();
    if (m_thumbs) m_thumbs->stop();
    QMainWindow::closeEvent(e);
}

// ---------- shortcuts -------------------------------------------------------

void MainWindow::wireShortcuts() {
    auto addSc = [this](const QKeySequence& k, std::function<void()> fn) {
        auto* sc = new QShortcut(k, this);
        sc->setContext(Qt::WindowShortcut);
        connect(sc, &QShortcut::activated, this, std::move(fn));
    };

    // Frame-accurate stepping (mpv conventions).
    addSc(QKeySequence(Qt::Key_Period),                  [this]{ m_player->frameStep(); });
    addSc(QKeySequence(Qt::Key_Comma),                   [this]{ m_player->frameBackStep(); });

    // Fine seek (1s) with Shift; coarse seek already on the Playback menu.
    addSc(QKeySequence(Qt::SHIFT | Qt::Key_Left),        [this]{ m_player->seekRelative(-1, true); });
    addSc(QKeySequence(Qt::SHIFT | Qt::Key_Right),       [this]{ m_player->seekRelative(+1, true); });

    // Previous / next keyframe — mirrors the ◀◀ / ▶▶ buttons.
    addSc(QKeySequence(Qt::CTRL  | Qt::Key_Left),
          [this]{ m_player->sendCommand({ "seek", "-5", "keyframes" }); });
    addSc(QKeySequence(Qt::CTRL  | Qt::Key_Right),
          [this]{ m_player->sendCommand({ "seek", "5",  "keyframes" }); });

    // Speed control — mpv keybinds: [ ] for ±0.1, BackSpace for reset.
    addSc(QKeySequence(Qt::Key_BracketLeft),             [this]{ m_player->stepSpeed(-0.10); });
    addSc(QKeySequence(Qt::Key_BracketRight),            [this]{ m_player->stepSpeed(+0.10); });
    addSc(QKeySequence(Qt::Key_Backspace),               [this]{ m_player->resetSpeed(); });

    // Audio / subtitle cycling.
    addSc(QKeySequence(Qt::Key_NumberSign),              [this]{ m_player->cycleAudio(+1); });
    addSc(QKeySequence(Qt::Key_J),                       [this]{ m_player->cycleSubtitle(+1); });
    addSc(QKeySequence(Qt::SHIFT | Qt::Key_J),           [this]{ m_player->cycleSubtitle(-1); });
    addSc(QKeySequence(Qt::Key_V),                       [this]{ m_player->toggleSubtitleVisibility(); });

    // Sub-delay adjust ±0.1 s.
    addSc(QKeySequence(Qt::Key_Z),
          [this]{ m_player->setSubtitleDelay(m_player->getProperty("sub-delay").toDouble() - 0.1); });
    addSc(QKeySequence(Qt::Key_X),
          [this]{ m_player->setSubtitleDelay(m_player->getProperty("sub-delay").toDouble() + 0.1); });

    // 字幕缩放 ±10%（Ctrl++ / Ctrl+- / Ctrl+= 三个都接受）。
    auto subScale = [this](double factor) {
        const double s = m_player->getProperty("sub-scale").toDouble();
        const double n = (s > 0 ? s : 1.0) * factor;
        m_player->setProperty("sub-scale", n);
        m_player->showText(QString("字幕缩放：%1").arg(n, 0, 'f', 2));
    };
    addSc(QKeySequence(Qt::CTRL | Qt::Key_Plus),   [subScale]{ subScale(1.10); });
    addSc(QKeySequence(Qt::CTRL | Qt::Key_Equal),  [subScale]{ subScale(1.10); });
    addSc(QKeySequence(Qt::CTRL | Qt::Key_Minus),  [subScale]{ subScale(1.0 / 1.10); });

    // Sub vertical position — mpv-style R/T (R = up, T = down).
    addSc(QKeySequence(Qt::Key_R), [this]{
        const double p = m_player->getProperty("sub-pos").toDouble();
        m_player->setProperty("sub-pos", qMax(0.0, p - 2.0));
    });
    addSc(QKeySequence(Qt::Key_T), [this]{
        const double p = m_player->getProperty("sub-pos").toDouble();
        m_player->setProperty("sub-pos", qMin(100.0, p + 2.0));
    });

    // Audio-delay adjust ±0.1 s.
    addSc(QKeySequence(Qt::CTRL | Qt::Key_Z),
          [this]{ m_player->setAudioDelay(m_player->getProperty("audio-delay").toDouble() - 0.1); });
    addSc(QKeySequence(Qt::CTRL | Qt::Key_X),
          [this]{ m_player->setAudioDelay(m_player->getProperty("audio-delay").toDouble() + 0.1); });

    // Volume increments via keyboard, mpv-style 9 / 0.
    addSc(QKeySequence(Qt::Key_9), [this]{
        double v = m_player->getProperty("volume").toDouble();
        m_player->setVolume(qBound(0.0, v - 5.0, 130.0));
    });
    addSc(QKeySequence(Qt::Key_0), [this]{
        double v = m_player->getProperty("volume").toDouble();
        m_player->setVolume(qBound(0.0, v + 5.0, 130.0));
    });

    // mpv stats overlay toggle.
    addSc(QKeySequence(Qt::SHIFT | Qt::Key_I), [this]{
        m_player->sendCommand({ "script-binding", "stats/display-stats-toggle" });
    });
}

// ---------- track menus -----------------------------------------------------

void MainWindow::onTracksChanged() { rebuildTrackMenus(); }

static QString trackLabel(const TrackInfo& t) {
    QStringList parts;
    parts << QString::number(t.id);
    if (!t.title.isEmpty()) parts << t.title;
    if (!t.lang.isEmpty())  parts << QString("[%1]").arg(t.lang);
    if (!t.codec.isEmpty()) parts << QString("(%1)").arg(t.codec);
    if (t.external) {
        if (!t.filename.isEmpty()) parts << QFileInfo(t.filename).fileName();
        parts << "(ext)";
    }
    return parts.join(' ');
}

void MainWindow::rebuildTrackMenus() {
    if (!m_menuAudio || !m_menuSubs) return;

    auto fill = [&](QMenu* menu, TrackInfo::Type type) {
        menu->clear();
        auto* group = new QActionGroup(menu);
        group->setExclusive(true);

        // 「禁用」始终在首位。
        auto* off = menu->addAction(tr("禁用"));
        off->setCheckable(true);
        group->addAction(off);
        connect(off, &QAction::triggered, this, [this, type]() {
            if (type == TrackInfo::Type::Audio)         m_player->setAudioTrack(0);
            else if (type == TrackInfo::Type::Subtitle) m_player->setSubtitleTrack(0);
        });

        bool anySelected = false;
        for (const auto& t : m_player->tracks()) {
            if (t.type != type) continue;
            auto* a = menu->addAction(trackLabel(t));
            a->setCheckable(true);
            a->setChecked(t.selected);
            if (t.selected) anySelected = true;
            group->addAction(a);
            const int id = t.id;
            connect(a, &QAction::triggered, this, [this, id, type]() {
                if (type == TrackInfo::Type::Audio)         m_player->setAudioTrack(id);
                else if (type == TrackInfo::Type::Subtitle) m_player->setSubtitleTrack(id);
            });
        }
        off->setChecked(!anySelected);

        if (type == TrackInfo::Type::Subtitle) {
            menu->addSeparator();
            menu->addAction(tr("加载外挂字幕..."), this, [this]() {
                const QString f = QFileDialog::getOpenFileName(this, tr("加载字幕"), m_lastOpenDir,
                    tr("字幕文件 (*.srt *.ass *.ssa *.sub *.vtt);;所有文件 (*.*)"));
                if (!f.isEmpty()) m_player->sendCommand({ "sub-add", f });
            });
            menu->addAction(tr("显示 / 隐藏字幕  (V)"), this,
                            [this]{ m_player->toggleSubtitleVisibility(); });

            menu->addSeparator();
            auto* mTime = menu->addMenu(tr("字幕时间轴（延时）"));
            mTime->addAction(tr("设为绝对延时（秒）…"), this, [this]() {
                bool ok = false;
                const double cur = m_player->getProperty("sub-delay").toDouble();
                const double v = QInputDialog::getDouble(
                    this, tr("字幕延时"), tr("延时（秒，正值表示字幕延后）："),
                    cur, -120.0, 120.0, 3, &ok);
                if (ok) {
                    m_player->setSubtitleDelay(v);
                    m_player->showText(QString("字幕延时：%1 秒").arg(v, 0, 'f', 3));
                }
            });
            mTime->addAction(tr("相对前进…（秒）"), this, [this]() {
                bool ok = false;
                const double n = QInputDialog::getDouble(
                    this, tr("字幕相对前进"), tr("前进秒数 n："), 1.0,
                    0.001, 3600.0, 3, &ok);
                if (ok) {
                    const double cur = m_player->getProperty("sub-delay").toDouble();
                    const double next = cur + n;
                    m_player->setSubtitleDelay(next);
                    m_player->showText(QString("字幕延时：%1 秒").arg(next, 0, 'f', 3));
                }
            });
            mTime->addAction(tr("相对后退…（秒）"), this, [this]() {
                bool ok = false;
                const double n = QInputDialog::getDouble(
                    this, tr("字幕相对后退"), tr("后退秒数 n："), 1.0,
                    0.001, 3600.0, 3, &ok);
                if (ok) {
                    const double cur = m_player->getProperty("sub-delay").toDouble();
                    const double next = cur - n;
                    m_player->setSubtitleDelay(next);
                    m_player->showText(QString("字幕延时：%1 秒").arg(next, 0, 'f', 3));
                }
            });
            menu->addSeparator();
            menu->addAction(tr("放大字幕  (Ctrl++)"),  this, [this]() {
                const double s = m_player->getProperty("sub-scale").toDouble();
                const double n = (s > 0 ? s : 1.0) * 1.1;
                m_player->setProperty("sub-scale", n);
                m_player->showText(QString("字幕缩放：%1").arg(n, 0, 'f', 2));
            });
            menu->addAction(tr("缩小字幕  (Ctrl+-)"),  this, [this]() {
                const double s = m_player->getProperty("sub-scale").toDouble();
                const double n = (s > 0 ? s : 1.0) / 1.1;
                m_player->setProperty("sub-scale", n);
                m_player->showText(QString("字幕缩放：%1").arg(n, 0, 'f', 2));
            });
            menu->addAction(tr("上移字幕  (R)"),       this, [this]() {
                const double p = m_player->getProperty("sub-pos").toDouble();
                m_player->setProperty("sub-pos", qMax(0.0, p - 2.0));
            });
            menu->addAction(tr("下移字幕  (T)"),       this, [this]() {
                const double p = m_player->getProperty("sub-pos").toDouble();
                m_player->setProperty("sub-pos", qMin(100.0, p + 2.0));
            });
            menu->addAction(tr("重置字幕（位置/大小/延时）"), this, [this]() {
                m_player->setProperty("sub-pos",   100.0);
                m_player->setProperty("sub-scale", 1.0);
                m_player->setProperty("sub-delay", 0.0);
                m_player->showText(QStringLiteral("字幕已重置"));
            });

            // --- 中日文老字幕乱码救场 ---
            menu->addSeparator();
            auto* mEnc = menu->addMenu(tr("字幕编码"));
            const QList<QPair<QString, QString>> encs {
                { "auto",         tr("自动识别（默认）") },
                { "utf-8",        tr("UTF-8") },
                { "gb18030",      tr("简体中文（GB18030）") },
                { "big5",         tr("繁体中文（Big5）") },
                { "shift_jis",    tr("日文（Shift-JIS）") },
                { "cp1252",       tr("西文（CP1252）") },
            };
            for (const auto& [code, label] : encs) {
                const QString c = code;
                mEnc->addAction(label, this, [this, c]() {
                    m_player->setProperty("sub-codepage", c);
                    // 字幕编码切换后必须 sub-reload 才会生效。
                    m_player->sendCommand({ "sub-reload" });
                    m_player->showText(QString("字幕编码：%1").arg(c));
                });
            }
        }
        if (type == TrackInfo::Type::Audio) {
            menu->addSeparator();
            menu->addAction(tr("加载外挂音轨..."), this, [this]() {
                const QString f = QFileDialog::getOpenFileName(this, tr("加载音轨"), m_lastOpenDir,
                    tr("音频文件 (*.mp3 *.flac *.aac *.m4a *.opus *.ogg *.wav);;所有文件 (*.*)"));
                if (!f.isEmpty()) m_player->sendCommand({ "audio-add", f });
            });
        }
    };

    fill(m_menuAudio, TrackInfo::Type::Audio);
    fill(m_menuSubs,  TrackInfo::Type::Subtitle);
}

// ---------- mpv state echoes -----------------------------------------------

void MainWindow::onMpvVolume(double percent) {
    Q_UNUSED(percent);
    // Already routed to ControlBar via signal; nothing else to do here.
}

void MainWindow::onMpvMute(bool muted) {
    Q_UNUSED(muted);
}

// ---------- recent files ---------------------------------------------------

void MainWindow::addRecent(const QString& path) {
    if (path.isEmpty()) return;
    m_recentFiles.removeAll(path);
    m_recentFiles.prepend(path);
    while (m_recentFiles.size() > 10) m_recentFiles.removeLast();
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu() {
    if (!m_menuRecent) return;
    m_menuRecent->clear();
    if (m_recentFiles.isEmpty()) {
        auto* none = m_menuRecent->addAction(tr("（空）"));
        none->setEnabled(false);
        return;
    }
    for (const QString& f : m_recentFiles) {
        QString shown = QFileInfo(f).fileName();
        if (shown.isEmpty()) shown = f;       // 网址（无文件名）
        auto* a = m_menuRecent->addAction(shown);
        a->setToolTip(f);
        const QString path = f;
        connect(a, &QAction::triggered, this, [this, path]() { openPath(path); });
    }
    m_menuRecent->addSeparator();
    m_menuRecent->addAction(tr("清空列表"), this, [this]() {
        m_recentFiles.clear();
        rebuildRecentMenu();
    });
}

// ---------- persistence ----------------------------------------------------

void MainWindow::loadSettings() {
    QSettings s;
    if (auto g = s.value("window/geometry").toByteArray(); !g.isEmpty())
        restoreGeometry(g);
    if (auto st = s.value("window/state").toByteArray(); !st.isEmpty())
        restoreState(st);

    const double vol  = s.value("player/volume", 100.0).toDouble();
    const bool   mute = s.value("player/mute",   false).toBool();
    m_player->setVolume(vol);
    m_player->setMute(mute);
    m_controls->setVolume(vol);
    m_controls->setMuted(mute);
    m_controls->setResumeRemember(s.value(QStringLiteral("player/resumeRemember"), false).toBool());

    m_lastOpenDir = s.value("io/lastOpenDir").toString();
    m_recentFiles = s.value("io/recentFiles").toStringList();
    rebuildRecentMenu();

    // MpvPlayer's baseline already seeded the pre-init properties from
    // QSettings("player/qualityPreset" / "player/hwAccel") inside its
    // constructor. Mirror the active selections onto the menu radios here.
    {
        const auto preset = m_player->qualityPreset();
        if (m_actQualNative)   m_actQualNative  ->setChecked(preset == MpvPlayer::QualityPreset::Native);
        if (m_actQualStandard) m_actQualStandard->setChecked(preset == MpvPlayer::QualityPreset::Standard);
        if (m_actQualHigh)     m_actQualHigh    ->setChecked(preset == MpvPlayer::QualityPreset::High);
        if (m_actQualUltra)    m_actQualUltra   ->setChecked(preset == MpvPlayer::QualityPreset::Ultra);

        const auto hw = m_player->hwAccel();
        if (m_actHwOff)      m_actHwOff     ->setChecked(hw == MpvPlayer::HwAccel::Off);
        if (m_actHwCopy)     m_actHwCopy    ->setChecked(hw == MpvPlayer::HwAccel::CopyMode);
        if (m_actHwZeroCopy) m_actHwZeroCopy->setChecked(hw == MpvPlayer::HwAccel::ZeroCopy);
    }

    // Advanced quality (HDR + picture-adjust + custom shaders) from QSettings.
    applyAdvancedQualitySettings();

    // Subtitle look — restored to mpv so user customisation survives restarts.
    if (s.contains("subs/scale")) m_player->setProperty("sub-scale", s.value("subs/scale").toDouble());
    if (s.contains("subs/pos"))   m_player->setProperty("sub-pos",   s.value("subs/pos").toDouble());
    if (s.contains("subs/font"))  m_player->setProperty("sub-font",  s.value("subs/font").toString());
    if (s.contains("subs/size"))  m_player->setProperty("sub-font-size", s.value("subs/size").toInt());
    if (s.contains("subs/codepage")) m_player->setProperty("sub-codepage", s.value("subs/codepage").toString());

    // Panel visibility + playlist contents.
    const bool stripOn    = s.value("view/stripVisible",    false).toBool();
    const bool playlistOn = s.value("view/playlistVisible", false).toBool();
    if (m_strip)    m_strip->setVisible(stripOn);
    if (m_playlist) m_playlist->setVisible(playlistOn);
    if (m_actToggleStrip)    m_actToggleStrip->setChecked(stripOn);
    if (m_actTogglePlaylist) m_actTogglePlaylist->setChecked(playlistOn);
    if (m_btnSidebar)        m_btnSidebar->setChecked(playlistOn);
    if (m_playlist) m_playlist->setItems(s.value("playlist/items").toStringList());
}

void MainWindow::saveSettings() {
    persistResumePosition();

    QSettings s;
    s.setValue("window/geometry", saveGeometry());
    s.setValue("window/state",    saveState());
    s.setValue("player/volume",   m_player->getProperty("volume").toDouble());
    s.setValue("player/mute",     m_player->getProperty("mute").toBool());
    s.setValue(QStringLiteral("player/resumeRemember"),
               m_controls && m_controls->isResumeRemember());
    s.setValue("io/lastOpenDir",  m_lastOpenDir);
    s.setValue("io/recentFiles",  m_recentFiles);

    s.setValue(QStringLiteral("player/qualityPreset"), int(m_player->qualityPreset()));

    s.setValue("subs/scale",      m_player->getProperty("sub-scale").toDouble());
    s.setValue("subs/pos",        m_player->getProperty("sub-pos").toDouble());
    s.setValue("subs/font",       m_player->getProperty("sub-font").toString());
    s.setValue("subs/size",       m_player->getProperty("sub-font-size").toInt());
    s.setValue("subs/codepage",   m_player->getProperty("sub-codepage").toString());

    s.setValue("view/stripVisible",    m_strip    && m_strip->isVisible());
    s.setValue("view/playlistVisible", m_playlist && m_playlist->isVisible());
    s.setValue("playlist/items",       m_playlist ? m_playlist->items() : QStringList());
}

QString MainWindow::mediaKeyForResume(const QString& path) {
    if (path.isEmpty()) return {};
    const QFileInfo fi(path);
    if (fi.exists() && fi.isFile()) {
        const QString c = fi.canonicalFilePath();
        return c.isEmpty() ? fi.absoluteFilePath() : c;
    }
    return path.trimmed();
}

static bool fileIsSiblingVideo(const QFileInfo& fi) {
    static const QStringList kExt{
        QStringLiteral("mp4"),  QStringLiteral("mkv"),  QStringLiteral("webm"),
        QStringLiteral("mov"),  QStringLiteral("avi"),  QStringLiteral("flv"),
        QStringLiteral("ts"),   QStringLiteral("m2ts"), QStringLiteral("wmv"),
        QStringLiteral("vob"),  QStringLiteral("m4v"),  QStringLiteral("mpeg"),
        QStringLiteral("mpg"),  QStringLiteral("3gp"),  QStringLiteral("ogv"),
    };
    return kExt.contains(fi.suffix().toLower());
}

void MainWindow::syncPlaylistWithSameFolder(const QString& path) {
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile() || !m_playlist) return;

    QDir dir(fi.absolutePath());
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    QStringList vids;
    for (const QFileInfo& e : entries) {
        if (fileIsSiblingVideo(e))
            vids.append(e.absoluteFilePath());
    }
    if (vids.isEmpty()) {
        m_playlist->notePlaying(path);
        return;
    }
    m_playlist->setItems(vids);
    m_playlist->notePlaying(path);
}

void MainWindow::tryApplyResume(double durationSec) {
    if (m_resumeAppliedForFile) return;
    if (!m_controls || !m_controls->isResumeRemember()) {
        m_resumeAppliedForFile = true;
        return;
    }
    if (m_resumeMediaKey.isEmpty()) {
        m_resumeAppliedForFile = true;
        return;
    }
    if (durationSec <= 0.25) return;

    m_resumeAppliedForFile = true;

    QSettings st;
    st.beginGroup(QStringLiteral("resumePositions"));
    const double saved = st.value(m_resumeMediaKey, -1.0).toDouble();
    st.endGroup();

    if (saved >= 3.0 && saved < durationSec - 12.0) {
        m_player->seekAbsolute(saved, true);
        m_player->showText(tr("已从上次停止位置继续播放"), 2000);
    }
}

void MainWindow::persistResumePosition() {
    if (!m_controls || !m_controls->isResumeRemember()) return;
    if (m_resumeMediaKey.isEmpty()) return;

    const double dur = m_player->getProperty("duration").toDouble();
    const double pos = m_player->getProperty("time-pos").toDouble();
    if (!qIsFinite(pos) || !qIsFinite(dur) || dur <= 0.0) return;
    if (pos < 4.0) return;

    QSettings st;
    st.beginGroup(QStringLiteral("resumePositions"));
    if (pos >= dur - 12.0) {
        st.remove(m_resumeMediaKey);
    } else {
        st.setValue(m_resumeMediaKey, pos);
    }
    st.endGroup();
}

// ---------- advanced quality: shaders + HDR + picture --------------------

QString MainWindow::shaderUserDir() const {
    // Writable per-user folder. Created on demand so the entry in the
    // submenu always works even before the user has dropped anything in.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dir  = base + QStringLiteral("/shaders");
    QDir().mkpath(dir);
    return dir;
}

void MainWindow::rebuildShaderMenu() {
    if (!m_menuShaders) return;

    // Preserve which shaders were currently enabled across rebuilds.
    QStringList enabledNames;
    for (auto* a : m_menuShaders->actions()) {
        if (a->isCheckable() && a->isChecked()) {
            enabledNames << a->data().toString();
        }
    }
    if (enabledNames.isEmpty()) {
        enabledNames = QSettings().value(QStringLiteral("shaders/enabled")).toStringList();
    }

    m_menuShaders->clear();

    const QString dir = shaderUserDir();
    QDir d(dir);
    const QStringList filters{ QStringLiteral("*.glsl"), QStringLiteral("*.hook") };
    const QFileInfoList files = d.entryInfoList(filters, QDir::Files, QDir::Name);

    if (files.isEmpty()) {
        auto* empty = m_menuShaders->addAction(tr("（着色器目录为空）"));
        empty->setEnabled(false);
    } else {
        for (const QFileInfo& fi : files) {
            auto* act = m_menuShaders->addAction(fi.fileName());
            act->setCheckable(true);
            act->setData(fi.fileName());
            act->setToolTip(fi.absoluteFilePath());
            if (enabledNames.contains(fi.fileName())) act->setChecked(true);
            connect(act, &QAction::toggled, this, [this](bool) { applyActiveShaders(); });
        }
    }

    m_menuShaders->addSeparator();
    auto* aClear = m_menuShaders->addAction(tr("禁用全部着色器"));
    connect(aClear, &QAction::triggered, this, [this]() {
        for (auto* a : m_menuShaders->actions()) {
            if (a->isCheckable()) a->setChecked(false);
        }
        applyActiveShaders();
    });

    auto* aOpenDir = m_menuShaders->addAction(tr("打开着色器目录…"));
    connect(aOpenDir, &QAction::triggered, this, [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(shaderUserDir()));
    });

    auto* aHelp = m_menuShaders->addAction(tr("着色器使用说明…"));
    connect(aHelp, &QAction::triggered, this, [this]() {
        QMessageBox::information(this, tr("自定义着色器"),
            tr("把 <b>.glsl</b> 着色器文件放到下面这个目录里，再回到"
               "「画质 → 自定义着色器」就能勾选启用：<br><br>"
               "<code>%1</code><br><br>"
               "<b>推荐组合（按用途）：</b><br>"
               "• <b>1080p → 4K 上采样</b>（最大化清晰度）：FSRCNNX_x2_16-0-4-1.glsl<br>"
               "• <b>4K → 1080p 下采样</b>（更高质细节）：SSimDownscaler.glsl<br>"
               "• <b>动漫专用</b>：Anime4K_*.glsl 系列<br>"
               "• <b>色度上采样</b>：KrigBilateral.glsl<br><br>"
               "从 mpv 社区获取这些 .glsl 文件（GitHub 搜文件名）。"
               "勾选多个时按勾选顺序串联。任何 mpv 兼容的 GLSL hook 着色器都可用。")
            .arg(shaderUserDir()));
    });
}

void MainWindow::applyActiveShaders(bool showOsd) {
    if (!m_menuShaders || !m_player) return;

    QStringList paths;
    QStringList names;
    const QString dir = shaderUserDir();
    for (auto* a : m_menuShaders->actions()) {
        if (a->isCheckable() && a->isChecked()) {
            const QString name = a->data().toString();
            names << name;
            paths << QDir::toNativeSeparators(dir + QLatin1Char('/') + name);
        }
    }

    // mpv expects a list-typed property — Windows separator is ';', POSIX ':'.
#ifdef Q_OS_WIN
    const QString joined = paths.join(QLatin1Char(';'));
#else
    const QString joined = paths.join(QLatin1Char(':'));
#endif
    m_player->setProperty("glsl-shaders", joined);
    QSettings().setValue(QStringLiteral("shaders/enabled"), names);

    if (!showOsd) return;
    if (names.isEmpty()) {
        m_player->showText(tr("着色器：已禁用"), 1500);
    } else {
        m_player->showText(tr("着色器：%1 个已启用").arg(names.size()), 1500);
    }
}

void MainWindow::applyAdvancedQualitySettings() {
    if (!m_player) return;
    QSettings s;

    // HDR / tone-mapping
    if (s.contains("hdr/toneMapping"))
        m_player->setProperty("tone-mapping",        s.value("hdr/toneMapping").toString());
    if (s.contains("hdr/toneMappingParam"))
        m_player->setProperty("tone-mapping-param",  s.value("hdr/toneMappingParam").toDouble());
    if (s.contains("hdr/targetPeak"))
        m_player->setProperty("target-peak",         s.value("hdr/targetPeak").toString());
    if (s.contains("hdr/peakPercentile"))
        m_player->setProperty("hdr-peak-percentile", s.value("hdr/peakPercentile").toDouble());
    if (s.contains("hdr/gamutMapping"))
        m_player->setProperty("gamut-mapping-mode",  s.value("hdr/gamutMapping").toString());
    if (s.contains("hdr/maxBoost"))
        m_player->setProperty("tone-mapping-max-boost", s.value("hdr/maxBoost").toDouble());

    // Picture-adjust
    if (s.contains("pic/brightness")) m_player->setProperty("brightness", s.value("pic/brightness").toInt());
    if (s.contains("pic/contrast"))   m_player->setProperty("contrast",   s.value("pic/contrast").toInt());
    if (s.contains("pic/saturation")) m_player->setProperty("saturation", s.value("pic/saturation").toInt());
    if (s.contains("pic/gamma"))      m_player->setProperty("gamma",      s.value("pic/gamma").toInt());
    if (s.contains("pic/hue"))        m_player->setProperty("hue",        s.value("pic/hue").toInt());
    if (s.contains("pic/sharpenX100"))
        m_player->setProperty("sharpen", s.value("pic/sharpenX100").toInt() / 100.0);

    // Denoise vf
    if (s.value("pic/denoise", false).toBool()) {
        const int level = s.value("pic/denoiseLevel", 1).toInt();
        const QString filter = (level == 1) ? QStringLiteral("hqdn3d=2:1:2:3")
                             : (level == 2) ? QStringLiteral("hqdn3d=4:3:6:4.5")
                                            : QStringLiteral("hqdn3d=6:4:9:6");
        m_player->setProperty("vf", filter);
    }

    // Custom shaders — populate-then-apply (do not require user to open menu).
    rebuildShaderMenu();
    applyActiveShaders(/*showOsd=*/false);
}

// ---------------------------------------------------------------------------
// AI subtitles entry point.
//
// The TranscriptionDialog is a *modeless* tool window so the user can keep
// the video playing (or fiddle with anything else) while a long job runs in
// the background worker thread. We keep one instance alive per main window
// via a static QPointer — re-clicking the menu simply brings the existing
// dialog to the front rather than spawning a parallel one.
// ---------------------------------------------------------------------------
void MainWindow::onAiGenerateSubtitles() {
    static QPointer<promp::ai::TranscriptionDialog> dlg;
    if (!dlg) {
        dlg = new promp::ai::TranscriptionDialog(m_player, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose, false);
        dlg->setWindowFlag(Qt::Window, true);
    }
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void MainWindow::onAiTranslateSubtitles() {
    static QPointer<promp::ai::TranslationDialog> dlg;
    if (!dlg) {
        dlg = new promp::ai::TranslationDialog(m_player, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose, false);
        dlg->setWindowFlag(Qt::Window, true);
    }
    // 优先：默认导出目录下的 `<片名>(日语原文).srt`；否则兼容旧版同目录 `<片名>.prompv.srt`。
    if (m_player) {
        const QString media = m_player->getProperty("path").toString();
        if (!media.isEmpty()) {
            const QFileInfo fi(media);
            const QString stem = promp::ai::videoStemFromMediaPath(media);
            const QString jaSrt =
                promp::ai::defaultSubtitleExportDir() + QLatin1Char('/')
                + stem + QStringLiteral("(日语原文).srt");
            if (QFile::exists(jaSrt)) {
                dlg->setInitialSourceSrt(jaSrt);
            } else {
                const QString legacy = fi.absolutePath() + QLatin1Char('/')
                                     + fi.completeBaseName()
                                     + QStringLiteral(".prompv.srt");
                if (QFile::exists(legacy)) dlg->setInitialSourceSrt(legacy);
            }
        }
    }
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

} // namespace promp
