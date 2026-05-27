// QualityAdvancedDialog.h
// Advanced picture controls in two tabs:
//
//   1. HDR / 色调映射 — pick tone-mapping algo, target peak (nits),
//      peak percentile, gamut-mapping, max-boost. All applied as
//      runtime mpv properties so users can A/B compare live.
//
//   2. 画质调节 — color (brightness/contrast/saturation/gamma/hue),
//      sharpening (mpv `sharpen` filter strength), denoise (hqdn3d via
//      the `vf` property). Sliders update mpv instantly; OK persists
//      to QSettings, Cancel reverts everything written this session.

#pragma once

#include <QDialog>
#include <QHash>
#include <QString>

class QComboBox;
class QDoubleSpinBox;
class QSlider;
class QCheckBox;
class QLabel;
class QTabWidget;

namespace promp {

class MpvPlayer;

class QualityAdvancedDialog : public QDialog {
    Q_OBJECT
public:
    explicit QualityAdvancedDialog(MpvPlayer* player, QWidget* parent = nullptr);

    /// Show the dialog opened on a specific tab. 0 = HDR, 1 = 画质调节.
    void selectTab(int index);

private slots:
    void applyAndAccept();
    void revertAndReject();
    void resetToDefaults();

    // Live application as user moves controls — gives instant A/B feedback.
    void onToneMappingChanged();
    void onTargetPeakChanged();
    void onPeakPercentileChanged();
    void onGamutChanged();
    void onMaxBoostChanged();
    void onToneMappingParamChanged();

    void onColorChanged();
    void onSharpenChanged();
    void onDenoiseToggled();
    void onDenoiseLevelChanged();

private:
    void buildUi();
    void loadFromPlayerAndSettings();   ///< populate widgets from current mpv state + QSettings
    void snapshotForRevert();           ///< remember property values to roll back on Cancel
    void writeProperty(const char* name, const QVariant& value);
    void writePropertyStr(const char* name, const QString& value);
    void applyDenoiseFilter();

    static QString denoiseFilterFor(int strength);  // 0..3 -> hqdn3d vf string ("" if off)

    MpvPlayer* m_player = nullptr;

    // Original property values, for Cancel.
    QHash<QByteArray, QVariant> m_snapshot;
    QString                     m_vfSnapshot;

    // HDR tab
    QComboBox*      m_cbToneMapping     = nullptr;
    QDoubleSpinBox* m_sbToneMappingParam= nullptr;
    QComboBox*      m_cbTargetPeak      = nullptr;
    QComboBox*      m_cbPeakPercentile  = nullptr;
    QComboBox*      m_cbGamutMapping    = nullptr;
    QDoubleSpinBox* m_sbMaxBoost        = nullptr;

    // Picture tab
    QSlider*   m_slBrightness = nullptr;
    QSlider*   m_slContrast   = nullptr;
    QSlider*   m_slSaturation = nullptr;
    QSlider*   m_slGamma      = nullptr;
    QSlider*   m_slHue        = nullptr;
    QLabel*    m_lblBrightness= nullptr;
    QLabel*    m_lblContrast  = nullptr;
    QLabel*    m_lblSaturation= nullptr;
    QLabel*    m_lblGamma     = nullptr;
    QLabel*    m_lblHue       = nullptr;

    QSlider*   m_slSharpen    = nullptr;
    QLabel*    m_lblSharpen   = nullptr;

    QCheckBox* m_chkDenoise   = nullptr;
    QSlider*   m_slDenoise    = nullptr;
    QLabel*    m_lblDenoise   = nullptr;

    QTabWidget* m_tabs = nullptr;
};

} // namespace promp
