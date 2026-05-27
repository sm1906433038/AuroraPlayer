// ControlBar.h
// Compact playback controls hosted at the bottom of the main window.

#pragma once

#include <QWidget>

class QLabel;
class QMenu;
class QPushButton;
class QSlider;
class QToolButton;
class QComboBox;

namespace promp {

class SeekBar;

class ControlBar : public QWidget {
    Q_OBJECT
public:
    explicit ControlBar(QWidget* parent = nullptr);

    [[nodiscard]] SeekBar* seekBar() const noexcept { return m_seek; }

    void setDuration(double seconds);
    void setPosition(double seconds);
    void setPaused(bool paused);
    void setVolume(double percent);
    void setMuted(bool muted);
    void setSpeed(double s);

    /// Wire the existing menubar's subtitle / VR menus to the in-control-bar
    /// shortcut buttons. After this call, clicking the button pops the menu.
    void setSubtitleMenu(QMenu* m);
    void setVrMenu(QMenu* m);

    void setResumeRemember(bool on);
    [[nodiscard]] bool isResumeRemember() const noexcept;

    void setMediaInfo(const QString& text);

signals:
    void playPauseClicked();
    void prevClicked();           ///< previous video in playlist
    void nextClicked();           ///< next video in playlist
    void prevKeyframeClicked();   ///< snap backward to previous keyframe
    void nextKeyframeClicked();   ///< snap forward to next keyframe
    void volumeChanged(double percent);
    void muteToggled();
    void speedChanged(double speed);
    void fullscreenToggled();

    /// 上次播放位置记忆（续播），开启后下次打开同一文件从上次时间点继续
    void resumeRememberToggled(bool enabled);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void buildLayout();
    static QString fmtTime(double s);

    SeekBar*     m_seek         = nullptr;
    QToolButton* m_btnPlay      = nullptr;
    QToolButton* m_btnPrev      = nullptr;     // |◀ previous video
    QToolButton* m_btnNext      = nullptr;     // ▶| next video
    QToolButton* m_btnPrevKey   = nullptr;     // ◀◀ previous keyframe
    QToolButton* m_btnNextKey   = nullptr;     // ▶▶ next keyframe
    QToolButton* m_btnSubs      = nullptr;     // 字幕 menu popup
    QToolButton* m_btnVr        = nullptr;     // VR menu popup
    QToolButton* m_btnResume    = nullptr;     // 续播（记忆上次位置）
    QLabel*      m_lblMediaInfo = nullptr;     // 分辨率/码率
    QToolButton* m_btnMute      = nullptr;
    QToolButton* m_btnFull      = nullptr;
    QSlider*     m_volume       = nullptr;
    QComboBox*   m_speedBox     = nullptr;
    QLabel*      m_lblPos       = nullptr;
    QLabel*      m_lblDur       = nullptr;

    double m_duration = 0.0;
    bool   m_muted    = false;
};

} // namespace promp
