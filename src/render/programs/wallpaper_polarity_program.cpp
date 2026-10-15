#include "render/programs/wallpaper_polarity_program.h"

#include <array>
#include <stdexcept>

namespace {

  constexpr char kVertexShader[] = R"(
precision highp float;
attribute vec2 a_position;
varying vec2 v_uv;

void main() {
    // Framebuffer row 0 samples texture row 0: the output keeps the source's row order.
    v_uv = a_position;
    gl_Position = vec4(a_position * 2.0 - 1.0, 0.0, 1.0);
}
)";

  constexpr char kFragmentShader[] = R"(
precision highp float;

uniform sampler2D u_source;
// The image's OKLab lightness range (2nd and 98th percentiles), and the range it is mapped onto.
uniform vec2 u_sourceRange;
uniform vec2 u_targetRange;

varying vec2 v_uv;

// In toe lightness: tones this close to the image's darkest count as its darkest (about
// 4/255 of sRGB noise over black).
const float kBlackPoint = 0.03;
// In toe lightness: below this, chroma fades out (it can't be seen there, and in a
// compressed image it is mostly noise).
const float kChromaFloor = 0.1;

vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 linearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

float cbrt(float x) {
    return sign(x) * pow(abs(x), 1.0 / 3.0);
}

vec3 linearToOklab(vec3 c) {
    float l = cbrt(0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b);
    float m = cbrt(0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b);
    float s = cbrt(0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b);
    return vec3(
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s);
}

vec3 oklabToLinear(vec3 lab) {
    float l = lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z;
    float m = lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z;
    float s = lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    return vec3(
        4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
}

// Okhsl's toe: OKLab lightness closer to CIELAB's.
float toe(float x) {
    float k3 = 1.206 / 1.03;
    float a = k3 * x - 0.206;
    return 0.5 * (a + sqrt(a * a + 4.0 * 0.03 * k3 * x));
}

float toeInverse(float x) {
    return (x * x + 0.206 * x) / ((1.206 / 1.03) * (x + 0.03));
}

bool inGamut(vec3 linear) {
    return all(greaterThanEqual(linear, vec3(-0.0001))) && all(lessThanEqual(linear, vec3(1.0001)));
}

void main() {
    vec4 color = texture2D(u_source, v_uv);
    vec3 lab = linearToOklab(srgbToLinear(color.rgb));
    float sourceToe = toe(max(lab.x, 0.0));

    float low = toe(u_sourceRange.x) + kBlackPoint;
    float high = max(toe(u_sourceRange.y), low + 0.001);
    float t = clamp((sourceToe - low) / (high - low), 0.0, 1.0);
    float targetLow = toe(u_targetRange.x);
    float targetHigh = toe(u_targetRange.y);
    float lightness = toeInverse(targetHigh - (targetHigh - targetLow) * t);

    vec2 ab = lab.yz * smoothstep(0.0, kChromaFloor, sourceToe);
    if (!inGamut(oklabToLinear(vec3(lightness, ab)))) {
        // The largest share of the chroma sRGB can show at this lightness and hue.
        float lowScale = 0.0;
        float highScale = 1.0;
        for (int i = 0; i < 16; ++i) {
            float scale = 0.5 * (lowScale + highScale);
            if (inGamut(oklabToLinear(vec3(lightness, ab * scale)))) {
                lowScale = scale;
            } else {
                highScale = scale;
            }
        }
        ab *= lowScale;
    }
    gl_FragColor = vec4(linearToSrgb(oklabToLinear(vec3(lightness, ab))), color.a);
}
)";

  // Framebuffer, viewport, program, texture and blend state, as they were before the pass.
  struct SavedState {
    GLint framebuffer = 0;
    std::array<GLint, 4> viewport{};
    GLint program = 0;
    GLint activeTexture = GL_TEXTURE0;
    GLint texture0 = 0;
    GLboolean blend = GL_FALSE;
    GLboolean scissor = GL_FALSE;
    GLint packAlignment = 4;

    static SavedState capture() {
      SavedState s;
      glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.framebuffer);
      glGetIntegerv(GL_VIEWPORT, s.viewport.data());
      glGetIntegerv(GL_CURRENT_PROGRAM, &s.program);
      glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTexture);
      glActiveTexture(GL_TEXTURE0);
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.texture0);
      s.blend = glIsEnabled(GL_BLEND);
      s.scissor = glIsEnabled(GL_SCISSOR_TEST);
      glGetIntegerv(GL_PACK_ALIGNMENT, &s.packAlignment);
      return s;
    }

    void restore() const {
      glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
      glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
      glUseProgram(static_cast<GLuint>(program));
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture0));
      glActiveTexture(static_cast<GLenum>(activeTexture));
      blend == GL_TRUE ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
      scissor == GL_TRUE ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
      glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
    }
  };

} // namespace

void WallpaperPolarityProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }
  m_program.create(kVertexShader, kFragmentShader);
  const GLuint id = m_program.id();
  m_positionLoc = glGetAttribLocation(id, "a_position");
  m_sourceLoc = glGetUniformLocation(id, "u_source");
  m_sourceRangeLoc = glGetUniformLocation(id, "u_sourceRange");
  m_targetRangeLoc = glGetUniformLocation(id, "u_targetRange");
  if (m_positionLoc < 0 || m_sourceLoc < 0 || m_sourceRangeLoc < 0 || m_targetRangeLoc < 0) {
    throw std::runtime_error("failed to query wallpaper polarity shader locations");
  }
}

void WallpaperPolarityProgram::destroy() { m_program.destroy(); }

void WallpaperPolarityProgram::abandon() noexcept { m_program.abandon(); }

bool WallpaperPolarityProgram::render(
    GLuint source, GLuint target, int width, int height, const wallpaper_polarity::Analysis& analysis,
    std::vector<std::uint8_t>* readback
) {
  ensureInitialized();
  const SavedState saved = SavedState::capture();

  GLuint framebuffer = 0;
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
  const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (complete) {
    static constexpr std::array<float, 12> kQuad = {
        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F,
    };
    const auto range = wallpaper_polarity::targetRangeFor(analysis);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(m_program.id());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    glUniform1i(m_sourceLoc, 0);
    glUniform2f(m_sourceRangeLoc, analysis.darkest, analysis.lightest);
    glUniform2f(m_targetRangeLoc, range.low, range.high);
    const auto position = static_cast<GLuint>(m_positionLoc);
    glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, kQuad.data());
    glEnableVertexAttribArray(position);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(position);

    if (readback != nullptr) {
      readback->resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, readback->data());
    }
  }

  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(saved.framebuffer));
  glDeleteFramebuffers(1, &framebuffer);
  saved.restore();
  return complete;
}
