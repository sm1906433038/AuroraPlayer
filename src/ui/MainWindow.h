// MainWindow.h
// Top-level window: menus, shortcuts, drag&drop, fullscreen, status.

#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QStringList>

class QVBoxLayout;
class QWidget;
class QResizeEvent;

namespace promp {

class MpvPlayer;
class VideoWidget;
class ControlBar;
class ThumbnailWorker;
class TimelineStrip;
class PlaylistPanel;
class VrController;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openPath(const QString& path);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void closeEvent(QCloseEvent* e) override;
    void changeEvent(QEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private slots:
    void onOpenFile();
    void onOpenUrl();
    void onToggleFullscreen();
    void onScreenshot();
    void onAbout();
    void onMpvLog(const QString& level, const QString& component, const QString& text);
    /// Starts the thumb thread on first real media load — must NOT run a second
    /// mpv during the main player's GL/render init or the two instances deadlock.
    void onMediaLoaded(const QString& path);
    void onTracksChanged();
    void onMpvVolume(double percent);
    void onMpvMute(bool muted);
    /// Opens the AI subtitle generation dialog (whisper.cpp local STT).
    void onAiGenerateSubtitles();
    /// Opens the AI subtitle translation dialog (OpenAI-compatible LLM API).
    void onAiTranslateSubtitles();

private:
    void buildMenus();
    void buildUi();
    void wireSignals();
    void wireShortcuts();
    void applyDarkPalette();
    void startThumbnailWorkerIfNeeded();
    void rebuildTrackMenus();
    void rebuildRecentMenu();
    void addRecent(const QString& path);
    void loadSettings();
    void saveSettings();
    void setOverlayAutoHide(bool enable);
    void wakeCursor();    // shows controls/cursor and re-arms idle timer
    /// After fullscreen / window-state changes, schedule extra GL repaints (no mpv context tear-down).
    void bumpVideoRedraw();
    /// Fullscreen: pin ControlBar to the bottom of the content area without layout
    /// height (overlaps video — avoids resize / "zoom" when auto-showing the bar).
    void layoutFullscreenControlBar();

    // ---- Custom GLSL shaders ----
    /// Scan `<appData>/AuroraPlayer/shaders` and rebuild the submenu of checkable
    /// shader entries. Called lazily when the user opens the submenu.
    void rebuildShaderMenu();
    /// Recompute the active shader list from currently-checked QActions
    /// and push the semicolon-joined paths into mpv `glsl-shaders`.
    /// `showOsd` suppresses the toast when called during startup.
    void applyActiveShaders(bool showOsd = true);
    /// Return the writable directory where users drop .glsl files.
    QString shaderUserDir() const;
    /// Apply HDR / picture-adjust QSettings on startup so values persist.
    void applyAdvancedQualitySettings();

    /// 续播：应用/保存上次播放位置
    void tryApplyResume(double durationSec);
    void persistResumePosition();
    static QString mediaKeyForResume(const QString& path);
    /// Replace playlist with all video files in the same folder (no subfolders).
    void syncPlaylistWithSameFolder(const QString& localFilePath);

    MpvPlayer*       m_player    = nullptr;
    VideoWidget*     m_video     = nullptr;
    ControlBar*      m_controls  = nullptr;
    ThumbnailWorker* m_thumbs    = nullptr;
    TimelineStrip*   m_strip     = nullptr;
    PlaylistPanel*   m_playlist  = nullptr;
    VrController*    m_vr        = nullptr;

    QAction* m_actVrOff    = nullptr;
    QAction* m_actVr360    = nullptr;
    QAction* m_actVr180    = nullptr;
    QAction* m_actStereoMono = nullptr;
    QAction* m_actStereoSBS  = nullptr;
    QAction* m_actStereoTB   = nullptr;

    // Dynamic track menus
    QMenu*   m_menuAudio     = nullptr;
    QMenu*   m_menuSubs      = nullptr;
    QMenu*   m_menuVr        = nullptr;
    QMenu*   m_menuRecent    = nullptr;

    QAction* m_actToggleStrip = nullptr;
    QAction* m_actTogglePlaylist = nullptr;

    // 画质（mpv 渲染管线档位）
    QAction* m_actQualNative   = nullptr;
    QAction* m_actQualStandard = nullptr;
    QAction* m_actQualHigh     = nullptr;
    QAction* m_actQualUltra    = nullptr;
    QMenu*   m_menuShaders     = nullptr;

    // 硬件加速
    QAction* m_actHwOff      = nullptr;
    QAction* m_actHwCopy     = nullptr;
    QAction* m_actHwZeroCopy = nullptr;

    QToolButton* m_btnSidebar = nullptr;

    QWidget*      m_contentArea  = nullptr;
    QVBoxLayout*  m_contentVLayout = nullptr;

    // Fullscreen auto-hide
    QTimer*  m_idleTimer     = nullptr;
    QTimer*  m_resumeTimer   = nullptr;   ///< periodic flush of resume position
    bool     m_overlayHidden = false;

    /// While dragging the seek bar: remember if playback was already paused.
    bool     m_wasPausedBeforeSeekScrub = false;

    // Borderless "logical" fullscreen — not Qt/OS full-screen (no DWM black flash).
    bool       m_fsActive   = false;
    QRect      m_fsSavedGeo;
    Qt::WindowStates m_fsSavedStates {};
    Qt::WindowFlags  m_fsSavedFlags  {};

    QString  m_lastOpenDir;
    QStringList m_recentFiles;

    /// 当前文件在“续播”设置中的键（本地文件为规范化路径，流为完整 URL）
    QString m_resumeMediaKey;
    bool    m_resumeAppliedForFile = false;

    bool m_thumbWorkerStarted = false;
};

} // namespace promp
