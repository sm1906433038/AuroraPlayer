// VrController.h
//
// Camera state holder for the VR / 360° post-pass.
//   * projection (off / 360° / 180° equirectangular)
//   * stereo layout (mono / side-by-side / top-bottom)
//   * yaw, pitch     (radians)
//   * fov            (radians, horizontal)
//
// This class no longer drives mpv directly. The actual spherical mapping is
// performed by VrRenderer inside VideoWidget::paintGL().

#pragma once

#include <QObject>

namespace promp {

class VrController : public QObject {
    Q_OBJECT
public:
    enum class Projection { None, Equirect360, Equirect180 };
    Q_ENUM(Projection)

    enum class Stereo { Mono, SBS, TB };
    Q_ENUM(Stereo)

    explicit VrController(QObject* parent = nullptr);

    [[nodiscard]] bool       isEnabled()  const noexcept { return m_projection != Projection::None; }
    [[nodiscard]] Projection projection() const noexcept { return m_projection; }
    [[nodiscard]] Stereo     stereo()     const noexcept { return m_stereo; }
    [[nodiscard]] float      yaw()        const noexcept { return m_yaw; }
    [[nodiscard]] float      pitch()      const noexcept { return m_pitch; }
    [[nodiscard]] float      fov()        const noexcept { return m_fov; }

signals:
    void changed();   ///< any parameter changed — host should schedule an update().

public slots:
    void setProjection(Projection p);
    void setStereo(Stereo s);
    void resetView();

    /// Pixel-deltas from the widget translated into yaw/pitch radians.
    void dragRotate(double dxPixels, double dyPixels);
    /// Wheel delta in Qt's 1/8-degree units — narrows / widens FOV (= zoom).
    void zoom(int wheelDelta);

private:
    Projection m_projection = Projection::None;
    Stereo     m_stereo     = Stereo::Mono;

    float m_yaw   = 0.0f;
    float m_pitch = 0.0f;
    float m_fov   = 1.30899694f;   // 75° in radians
};

} // namespace promp
