#include "VrController.h"

#include <QtMath>
#include <algorithm>

namespace promp {

namespace {
constexpr float kHalfPi = float(M_PI_2);
constexpr float kFovMin = 0.1745329f;   // 10°
constexpr float kFovMax = 2.6179939f;   // 150°
} // namespace

VrController::VrController(QObject* parent) : QObject(parent) {}

void VrController::setProjection(Projection p) {
    if (p == m_projection) return;
    m_projection = p;
    m_yaw = m_pitch = 0.0f;
    m_fov = 1.30899694f;
    emit changed();
}

void VrController::setStereo(Stereo s) {
    if (s == m_stereo) return;
    m_stereo = s;
    emit changed();
}

void VrController::resetView() {
    m_yaw = m_pitch = 0.0f;
    m_fov = 1.30899694f;
    emit changed();
}

void VrController::dragRotate(double dxPixels, double dyPixels) {
    if (!isEnabled()) return;
    // Sensitivity scales with FOV: at narrow FOV (zoomed in) the same pixel
    // delta should rotate less, mirroring how an optical zoom feels.
    const float kBase = 0.0035f;
    const float sens  = kBase * (m_fov / 1.30899694f);

    m_yaw   -= float(dxPixels) * sens;
    m_pitch -= float(dyPixels) * sens;

    // Wrap yaw into [-PI, PI] so it stays well-behaved over long drags.
    while (m_yaw >  float(M_PI)) m_yaw -= 2.0f * float(M_PI);
    while (m_yaw < -float(M_PI)) m_yaw += 2.0f * float(M_PI);
    // Clamp pitch so the camera doesn't flip over the poles.
    m_pitch = std::clamp(m_pitch, -kHalfPi + 0.01f, kHalfPi - 0.01f);

    emit changed();
}

void VrController::zoom(int wheelDelta) {
    if (!isEnabled()) return;
    // Each notch (120 units) scales FOV by ~0.9 / 1.1.
    const float factor = std::pow(0.999f, float(wheelDelta));
    m_fov = std::clamp(m_fov * factor, kFovMin, kFovMax);
    emit changed();
}

} // namespace promp
