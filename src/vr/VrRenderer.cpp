#include "VrRenderer.h"

#include <QByteArray>
#include <QDebug>

namespace promp {

// ---------- shader sources ---------------------------------------------------

namespace {

constexpr const char* kVertSrc = R"(#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vNDC;
void main() {
    vNDC = aPos;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

constexpr const char* kFragSrc = R"(#version 330 core
in  vec2 vNDC;
out vec4 fragColor;

uniform sampler2D uTex;
uniform float uYaw;          // radians
uniform float uPitch;        // radians
uniform float uFov;          // radians (horizontal)
uniform float uAspect;       // viewport width / height
uniform int   uProj;         // 0 = 360 equirect, 1 = 180 equirect
uniform int   uStereoCrop;   // 0 mono, 1 SBS (left half), 2 TB (top half)

const float PI = 3.14159265358979323846;

void main() {
    // Camera-space view direction: camera looks down +Z, screen plane at z=1.
    float tanHalf = tan(uFov * 0.5);
    vec3 dir = normalize(vec3(vNDC.x * tanHalf * uAspect,
                              vNDC.y * tanHalf,
                              1.0));

    // Apply pitch (rotation about X) then yaw (rotation about Y).
    float cp = cos(uPitch), sp = sin(uPitch);
    vec3 d1 = vec3(dir.x,
                   dir.y * cp - dir.z * sp,
                   dir.y * sp + dir.z * cp);

    float cy = cos(uYaw), sy = sin(uYaw);
    vec3 d2 = vec3( d1.x * cy + d1.z * sy,
                    d1.y,
                   -d1.x * sy + d1.z * cy);

    // Direction → (longitude, latitude).
    float lon = atan(d2.x, d2.z);                 // [-PI, PI]
    float lat = asin(clamp(d2.y, -1.0, 1.0));     // [-PI/2, PI/2]

    vec2 uv;
    if (uProj == 1) {
        // 180° equirect: rear hemisphere has no pixels — paint black.
        if (abs(lon) > PI * 0.5) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
        uv.x = lon / PI + 0.5;                    // [-PI/2,PI/2] → [0,1]
    } else {
        uv.x = lon / (2.0 * PI) + 0.5;            // [-PI,PI] → [0,1]
    }

    // Latitude → texture v. mpv renders into the FBO with FLIP_Y=1 so GL
    // row order matches the on-screen 2D path; combined with our sphere
    // mapping the panorama was appearing upside-down without this flip.
    uv.y = 0.5 + lat / PI;

    // Stereo: keep only the LEFT eye (SBS) or TOP eye (TB) for flat-screen view.
    if (uStereoCrop == 1) {
        uv.x *= 0.5;
    } else if (uStereoCrop == 2) {
        uv.y *= 0.5;
    }

    fragColor = texture(uTex, uv);
}
)";

unsigned int compileShader(QOpenGLFunctions_3_3_Core* gl, unsigned int type, const char* src) {
    unsigned int sh = gl->glCreateShader(type);
    gl->glShaderSource(sh, 1, &src, nullptr);
    gl->glCompileShader(sh);
    int ok = 0;
    gl->glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]{};
        gl->glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        qWarning("VrRenderer: shader compile failed: %s", log);
        gl->glDeleteShader(sh);
        return 0;
    }
    return sh;
}

} // namespace

// ---------- public API ------------------------------------------------------

bool VrRenderer::initialize() {
    if (m_program) return true;
    if (!initializeOpenGLFunctions()) {
        qWarning("VrRenderer: failed to bind GL 3.3 core functions");
        return false;
    }
    if (!buildProgram()) return false;

    // Fullscreen quad in NDC (two triangles).
    const float quad[] = {
        -1.f, -1.f,
         1.f, -1.f,
         1.f,  1.f,
        -1.f, -1.f,
         1.f,  1.f,
        -1.f,  1.f,
    };
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);

    return true;
}

bool VrRenderer::buildProgram() {
    unsigned int vs = compileShader(this, GL_VERTEX_SHADER,   kVertSrc);
    unsigned int fs = compileShader(this, GL_FRAGMENT_SHADER, kFragSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glLinkProgram(m_program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    int ok = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048]{};
        glGetProgramInfoLog(m_program, sizeof(log), nullptr, log);
        qWarning("VrRenderer: program link failed: %s", log);
        glDeleteProgram(m_program);
        m_program = 0;
        return false;
    }

    u_tex        = glGetUniformLocation(m_program, "uTex");
    u_yaw        = glGetUniformLocation(m_program, "uYaw");
    u_pitch      = glGetUniformLocation(m_program, "uPitch");
    u_fov        = glGetUniformLocation(m_program, "uFov");
    u_aspect     = glGetUniformLocation(m_program, "uAspect");
    u_proj       = glGetUniformLocation(m_program, "uProj");
    u_stereoCrop = glGetUniformLocation(m_program, "uStereoCrop");
    return true;
}

void VrRenderer::shutdown() {
    if (m_program) { glDeleteProgram(m_program);     m_program = 0; }
    if (m_vbo)     { glDeleteBuffers(1, &m_vbo);     m_vbo     = 0; }
    if (m_vao)     { glDeleteVertexArrays(1, &m_vao);m_vao     = 0; }
    if (m_texture) { glDeleteTextures(1, &m_texture);m_texture = 0; }
    if (m_fbo)     { glDeleteFramebuffers(1, &m_fbo);m_fbo     = 0; }
    m_fboW = m_fboH = 0;
}

void VrRenderer::resizeIntermediate(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (w == m_fboW && h == m_fboH && m_fbo) return;

    if (!m_texture) glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Wrap horizontally so the seam at lon = ±PI samples cleanly.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (!m_fbo) glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_texture, 0);

    const auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        qWarning("VrRenderer: framebuffer incomplete: 0x%x", unsigned(status));
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    m_fboW = w;
    m_fboH = h;
}

void VrRenderer::draw(int viewportW, int viewportH,
                      VrController::Projection proj,
                      VrController::Stereo stereo,
                      float yaw, float pitch, float fov) {
    if (!m_program || !m_texture) return;

    glViewport(0, 0, viewportW, viewportH);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUseProgram(m_program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glUniform1i (u_tex,        0);
    glUniform1f (u_yaw,        yaw);
    glUniform1f (u_pitch,      pitch);
    glUniform1f (u_fov,        fov);
    glUniform1f (u_aspect,     viewportH > 0 ? float(viewportW) / float(viewportH) : 1.0f);
    glUniform1i (u_proj,       proj == VrController::Projection::Equirect180 ? 1 : 0);

    int stereoCrop = 0;
    if      (stereo == VrController::Stereo::SBS) stereoCrop = 1;
    else if (stereo == VrController::Stereo::TB)  stereoCrop = 2;
    glUniform1i (u_stereoCrop, stereoCrop);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

} // namespace promp
