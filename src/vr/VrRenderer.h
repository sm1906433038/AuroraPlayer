// VrRenderer.h
//
// A self-contained GL post-pass that:
//   1. Owns an intermediate FBO (colour-attached RGBA8 texture) into which
//      libmpv renders the decoded equirectangular frame.
//   2. Owns a small shader pipeline (fullscreen quad + sphere mapping
//      fragment shader) that samples that FBO with a virtual camera and
//      writes the result to the currently bound framebuffer.
//
// Threading: all GL calls happen on the GUI thread inside VideoWidget's
// initializeGL() / paintGL() / resizeGL(). The owning QOpenGLWidget must
// `makeCurrent()` before calling any method here.

#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include <QSize>

#include "VrController.h"

namespace promp {

class VrRenderer : protected QOpenGLFunctions_3_3_Core {
public:
    VrRenderer()  = default;
    ~VrRenderer() = default;

    bool initialize();     ///< compiles shaders, allocates VAO/VBO. GL ctx must be current.
    void shutdown();       ///< frees everything. GL ctx must be current.
    [[nodiscard]] bool ready() const noexcept { return m_program != 0; }

    /// Ensure the intermediate FBO is sized at least `w` x `h`. No-op if already.
    void resizeIntermediate(int w, int h);

    [[nodiscard]] unsigned int fboId()  const noexcept { return m_fbo; }
    [[nodiscard]] int          fboW()   const noexcept { return m_fboW; }
    [[nodiscard]] int          fboH()   const noexcept { return m_fboH; }

    /// Apply the post-pass: read the intermediate texture, write the
    /// projection result to the bound framebuffer at (viewportW × viewportH).
    void draw(int viewportW, int viewportH,
              VrController::Projection proj,
              VrController::Stereo stereo,
              float yaw, float pitch, float fov);

private:
    bool buildProgram();

    unsigned int m_program  = 0;
    unsigned int m_vao      = 0;
    unsigned int m_vbo      = 0;

    unsigned int m_fbo      = 0;
    unsigned int m_texture  = 0;
    int          m_fboW     = 0;
    int          m_fboH     = 0;

    // uniform locations cache
    int u_tex          = -1;
    int u_yaw          = -1;
    int u_pitch        = -1;
    int u_fov          = -1;
    int u_aspect       = -1;
    int u_proj         = -1;
    int u_stereoCrop   = -1;
};

} // namespace promp
