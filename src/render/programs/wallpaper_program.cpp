#include "render/programs/wallpaper_program.h"

#include "render/core/wallpaper_polarity.h"
#include "render/programs/wallpaper_sampling_glsl.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

  constexpr char kVertexShader[] = R"(
precision highp float;
attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec2 u_quad_size;
uniform mat3 u_transform;
varying vec2 v_texcoord;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    v_texcoord = a_position;
    vec3 pixel = u_transform * vec3(a_position * u_quad_size, 1.0);
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  constexpr char kCommonPrefix[] = R"(
precision highp float;

uniform sampler2D u_source1;
uniform sampler2D u_source2;
uniform float u_sourceKind1;
uniform float u_sourceKind2;
uniform vec4 u_sourceColor1;
uniform vec4 u_sourceColor2;
uniform float u_progress;
uniform float u_imageWidth1;
uniform float u_imageHeight1;
uniform float u_imageWidth2;
uniform float u_imageHeight2;
uniform vec4 u_fillColor;
// Lightness inversion per source (wallpaper_polarity.h): x inverts when 1, y and z are the
// image's lightness range; the target is the range it is mapped onto, reversed.
uniform vec4 u_polarity1;
uniform vec4 u_polarity2;
uniform vec2 u_target1;
uniform vec2 u_target2;
// Largest chroma sRGB can show, as a fraction of 0.4: hue across, lightness down.
uniform sampler2D u_chromaTable;

varying vec2 v_texcoord;
)";

  constexpr char kCommonSuffix[] = R"(
vec4 sampleWithFillMode(sampler2D tex, vec2 uv, float imgWidth, float imgHeight) {
    vec2 transformedUV = calculateWallpaperUV(uv, imgWidth, imgHeight);

    if (u_fillMode > 3.5 && u_fillMode < 4.5) {
        return texture2D(tex, fract(transformedUV));
    }

    if (wallpaperUVOutside(transformedUV)) {
        return u_fillColor;
    }

    return texture2D(tex, transformedUV);
}

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

// The image's lightness stretched from its own range onto the target one, reversed, with the
// chroma it had where sRGB can show it at the new lightness, and the most it can show there
// (same hue) where it can't.
vec4 applyPolarity(vec4 color, vec4 polarity, vec2 target) {
    if (polarity.x < 0.5) {
        return color;
    }
    vec3 lab = linearToOklab(srgbToLinear(color.rgb));
    float t = clamp((lab.x - polarity.y) / max(polarity.z - polarity.y, 0.001), 0.0, 1.0);
    float lightness = target.y - (target.y - target.x) * t;
    float chroma = length(lab.yz);
    float hue = fract(atan(lab.z, lab.y) / 6.28318530718 + 1.0);
    // Nearest table entry, as the CPU side rounds (wallpaper_polarity.cpp).
    float column = mod(floor(hue * 360.0 + 0.5), 360.0);
    float row = floor(lightness * 255.0 + 0.5);
    float maxChroma = texture2D(u_chromaTable, vec2((column + 0.5) / 360.0, (row + 0.5) / 256.0)).r * 0.4;
    float scale = chroma > maxChroma ? maxChroma / max(chroma, 1e-6) : 1.0;
    return vec4(linearToSrgb(oklabToLinear(vec3(lightness, lab.yz * scale))), color.a);
}

vec4 sampleSource(
    sampler2D tex, vec2 uv, float imgWidth, float imgHeight, float sourceKind, vec4 sourceColor, vec4 polarity,
    vec2 target) {
    if (sourceKind > 0.5) {
        return sourceColor;
    }
    return applyPolarity(sampleWithFillMode(tex, uv, imgWidth, imgHeight), polarity, target);
}
)";

  constexpr char kFadeFragment[] = R"(
void main() {
    vec2 uv = v_texcoord;
    vec4 color1 = sampleSource(u_source1, uv, u_imageWidth1, u_imageHeight1, u_sourceKind1, u_sourceColor1, u_polarity1, u_target1);
    vec4 color2 = sampleSource(u_source2, uv, u_imageWidth2, u_imageHeight2, u_sourceKind2, u_sourceColor2, u_polarity2, u_target2);
    gl_FragColor = mix(color1, color2, u_progress);
}
)";

  constexpr char kWipeFragment[] = R"(
uniform float u_direction;
uniform float u_smoothness;

void main() {
    vec2 uv = v_texcoord;
    vec4 color1 = sampleSource(u_source1, uv, u_imageWidth1, u_imageHeight1, u_sourceKind1, u_sourceColor1, u_polarity1, u_target1);
    vec4 color2 = sampleSource(u_source2, uv, u_imageWidth2, u_imageHeight2, u_sourceKind2, u_sourceColor2, u_polarity2, u_target2);

    float mappedSmoothness = mix(0.001, 0.5, u_smoothness * u_smoothness);
    float extendedProgress = u_progress * (1.0 + 2.0 * mappedSmoothness) - mappedSmoothness;
    float edge = 0.0;
    float factor = 0.0;

    if (u_direction < 0.5) {
        edge = 1.0 - extendedProgress;
        factor = smoothstep(edge - mappedSmoothness, edge + mappedSmoothness, uv.x);
        gl_FragColor = mix(color1, color2, factor);
    }
    else if (u_direction < 1.5) {
        edge = extendedProgress;
        factor = smoothstep(edge - mappedSmoothness, edge + mappedSmoothness, uv.x);
        gl_FragColor = mix(color2, color1, factor);
    }
    else if (u_direction < 2.5) {
        edge = 1.0 - extendedProgress;
        factor = smoothstep(edge - mappedSmoothness, edge + mappedSmoothness, uv.y);
        gl_FragColor = mix(color1, color2, factor);
    }
    else {
        edge = extendedProgress;
        factor = smoothstep(edge - mappedSmoothness, edge + mappedSmoothness, uv.y);
        gl_FragColor = mix(color2, color1, factor);
    }
}
)";

  constexpr char kDiscFragment[] = R"(
uniform float u_smoothness;
uniform float u_centerX;
uniform float u_centerY;
uniform float u_aspectRatio;

void main() {
    vec2 uv = v_texcoord;
    vec4 color1 = sampleSource(u_source1, uv, u_imageWidth1, u_imageHeight1, u_sourceKind1, u_sourceColor1, u_polarity1, u_target1);
    vec4 color2 = sampleSource(u_source2, uv, u_imageWidth2, u_imageHeight2, u_sourceKind2, u_sourceColor2, u_polarity2, u_target2);

    float mappedSmoothness = mix(0.001, 0.5, u_smoothness * u_smoothness);

    vec2 center = vec2(u_centerX, u_centerY);
    vec2 aspectUV = vec2(uv.x * u_aspectRatio, uv.y);
    vec2 aspectCenter = vec2(center.x * u_aspectRatio, center.y);
    float dist = distance(aspectUV, aspectCenter);

    // Calculate max distance to corners
    float maxDist = 0.0;
    maxDist = max(maxDist, distance(aspectCenter, vec2(0.0, 0.0)));
    maxDist = max(maxDist, distance(aspectCenter, vec2(u_aspectRatio, 0.0)));
    maxDist = max(maxDist, distance(aspectCenter, vec2(0.0, 1.0)));
    maxDist = max(maxDist, distance(aspectCenter, vec2(u_aspectRatio, 1.0)));

    float radius = u_progress * (maxDist + 2.0 * mappedSmoothness) - mappedSmoothness;
    float factor = smoothstep(radius - mappedSmoothness, radius + mappedSmoothness, dist);
    gl_FragColor = mix(color2, color1, factor);
}
)";

  constexpr char kStripesFragment[] = R"(
uniform float u_smoothness;
uniform float u_aspectRatio;
uniform float u_stripeCount;
uniform float u_angle;

void main() {
    vec2 uv = v_texcoord;
    vec4 color1 = sampleSource(u_source1, uv, u_imageWidth1, u_imageHeight1, u_sourceKind1, u_sourceColor1, u_polarity1, u_target1);
    vec4 color2 = sampleSource(u_source2, uv, u_imageWidth2, u_imageHeight2, u_sourceKind2, u_sourceColor2, u_polarity2, u_target2);

    float mappedSmoothness = mix(0.001, 0.3, u_smoothness * u_smoothness);

    float rad = radians(u_angle);
    float cosA = cos(rad);
    float sinA = sin(rad);
    vec2 aspectUV = vec2(uv.x * u_aspectRatio, uv.y);

    float stripeCoord = aspectUV.x * cosA + aspectUV.y * sinA;
    float perpCoord = -aspectUV.x * sinA + aspectUV.y * cosA;

    float totalStripes = u_stripeCount;
    float maxStripeCoord = u_aspectRatio * abs(cosA) + abs(sinA);
    float stripeWidth = maxStripeCoord / totalStripes;

    float stripePos = stripeCoord / stripeWidth;
    float stripeIndex = floor(stripePos);
    float isOdd = mod(stripeIndex, 2.0);

    float maxPerp = u_aspectRatio * abs(sinA) + abs(cosA);
    float normalizedPerp = perpCoord / maxPerp;

    float delay = abs(normalizedPerp) * 0.3;
    float localProgress = clamp((u_progress - delay) / (1.0 - delay), 0.0, 1.0);

    float edge;
    float localFrac = fract(stripePos);
    float factor;
    if (isOdd > 0.5) {
        edge = mix(1.0 + mappedSmoothness, -mappedSmoothness, localProgress);
        factor = smoothstep(edge - mappedSmoothness, edge + mappedSmoothness, localFrac);
        gl_FragColor = mix(color1, color2, factor);
    } else {
        edge = mix(-mappedSmoothness, 1.0 + mappedSmoothness, localProgress);
        factor = smoothstep(edge - mappedSmoothness, edge + mappedSmoothness, localFrac);
        gl_FragColor = mix(color2, color1, factor);
    }
}
)";

  constexpr char kZoomFragment[] = R"(
void main() {
    vec2 uv = v_texcoord;

    float zoom = 0.15;

    // Old image zooms in (grows toward viewer) while fading out
    float scale1 = 1.0 + zoom * u_progress;
    vec2 uv1 = (uv - 0.5) / scale1 + 0.5;

    // New image arrives slightly zoomed in, returns to normal scale while fading in
    float scale2 = 1.0 + zoom * (1.0 - u_progress);
    vec2 uv2 = (uv - 0.5) / scale2 + 0.5;

    vec4 color1 = sampleSource(u_source1, uv1, u_imageWidth1, u_imageHeight1, u_sourceKind1, u_sourceColor1, u_polarity1, u_target1);
    vec4 color2 = sampleSource(u_source2, uv2, u_imageWidth2, u_imageHeight2, u_sourceKind2, u_sourceColor2, u_polarity2, u_target2);

    gl_FragColor = mix(color1, color2, u_progress);
}
)";

  constexpr char kHoneycombFragment[] = R"(
uniform float u_cellSize;
uniform float u_centerX;
uniform float u_centerY;
uniform float u_aspectRatio;
uniform float u_smoothness;

vec2 hexRound(float q, float r) {
    float x = q;
    float z = r;
    float y = -x - z;

    float rx = floor(x + 0.5);
    float ry = floor(y + 0.5);
    float rz = floor(z + 0.5);

    float dx = abs(rx - x);
    float dy = abs(ry - y);
    float dz = abs(rz - z);

    if (dx > dy && dx > dz) {
        rx = -ry - rz;
    } else if (dy > dz) {
        ry = -rx - rz;
    } else {
        rz = -rx - ry;
    }

    return vec2(rx, rz);
}

void main() {
    vec2 uv = v_texcoord;
    vec4 color1 = sampleSource(u_source1, uv, u_imageWidth1, u_imageHeight1, u_sourceKind1, u_sourceColor1, u_polarity1, u_target1);
    vec4 color2 = sampleSource(u_source2, uv, u_imageWidth2, u_imageHeight2, u_sourceKind2, u_sourceColor2, u_polarity2, u_target2);

    float mappedSmoothness = mix(0.001, 0.5, u_smoothness * u_smoothness);

    float size = u_cellSize;
    vec2 aspectUV = vec2(uv.x * u_aspectRatio, uv.y);

    float q = (aspectUV.x * (2.0 / 3.0)) / size;
    float r = ((-aspectUV.x / 3.0) + (sqrt(3.0) / 3.0) * aspectUV.y) / size;

    vec2 hexCoord = hexRound(q, r);

    // Convert back to screen space for cell center
    float cellCenterX = (hexCoord.x * 1.5) * size;
    float cellCenterY = (hexCoord.x * sqrt(3.0) / 2.0 + hexCoord.y * sqrt(3.0)) * size;
    vec2 cellCenter = vec2(cellCenterX, cellCenterY);

    vec2 waveOrigin = vec2(u_centerX * u_aspectRatio, u_centerY);
    float dist = distance(cellCenter, waveOrigin);

    float maxDist = 0.0;
    maxDist = max(maxDist, distance(waveOrigin, vec2(0.0, 0.0)));
    maxDist = max(maxDist, distance(waveOrigin, vec2(u_aspectRatio, 0.0)));
    maxDist = max(maxDist, distance(waveOrigin, vec2(0.0, 1.0)));
    maxDist = max(maxDist, distance(waveOrigin, vec2(u_aspectRatio, 1.0)));

    float radius = u_progress * (maxDist + 2.0 * mappedSmoothness) - mappedSmoothness;
    float factor = smoothstep(radius - mappedSmoothness, radius + mappedSmoothness, dist);
    gl_FragColor = mix(color2, color1, factor);
}
)";

} // namespace

void WallpaperProgram::ensureInitialized() {
  if (m_programs[0].program.isValid()) {
    return;
  }

  initProgram(static_cast<std::size_t>(WallpaperTransition::Fade), kFadeFragment);
  initProgram(static_cast<std::size_t>(WallpaperTransition::Wipe), kWipeFragment);
  initProgram(static_cast<std::size_t>(WallpaperTransition::Disc), kDiscFragment);
  initProgram(static_cast<std::size_t>(WallpaperTransition::Stripes), kStripesFragment);
  initProgram(static_cast<std::size_t>(WallpaperTransition::Zoom), kZoomFragment);
  initProgram(static_cast<std::size_t>(WallpaperTransition::Honeycomb), kHoneycombFragment);
  initChromaTable();
}

void WallpaperProgram::destroy() {
  for (auto& pd : m_programs) {
    pd.program.destroy();
  }
  if (m_chromaTable != 0) {
    glDeleteTextures(1, &m_chromaTable);
    m_chromaTable = 0;
  }
}

void WallpaperProgram::abandon() noexcept {
  for (auto& pd : m_programs) {
    pd.program.abandon();
  }
  m_chromaTable = 0;
}

void WallpaperProgram::initChromaTable() {
  using namespace wallpaper_polarity;
  const std::vector<float>& table = maxChromaTable();
  // Rounded down, so the shader never allows more chroma than sRGB can show.
  std::vector<std::uint8_t> bytes(table.size());
  std::ranges::transform(table, bytes.begin(), [](float chroma) {
    return static_cast<std::uint8_t>(std::clamp(std::floor(chroma / kTableMaxChroma * 255.0F), 0.0F, 255.0F));
  });
  glGenTextures(1, &m_chromaTable);
  glBindTexture(GL_TEXTURE_2D, m_chromaTable);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(
      GL_TEXTURE_2D, 0, GL_LUMINANCE, kTableHues, kTableLightness, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, bytes.data()
  );
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void WallpaperProgram::initProgram(std::size_t index, const char* fragSource) {
  const std::string fullFrag =
      std::string(kCommonPrefix) + std::string(wallpaper_shader::kSamplingSource) + kCommonSuffix + fragSource;

  auto& pd = m_programs[index];
  pd.program.create(kVertexShader, fullFrag.c_str());

  auto id = pd.program.id();
  pd.positionLoc = glGetAttribLocation(id, "a_position");
  pd.surfaceSizeLoc = glGetUniformLocation(id, "u_surface_size");
  pd.quadSizeLoc = glGetUniformLocation(id, "u_quad_size");
  pd.transformLoc = glGetUniformLocation(id, "u_transform");
  pd.source1Loc = glGetUniformLocation(id, "u_source1");
  pd.source2Loc = glGetUniformLocation(id, "u_source2");
  pd.sourceKind1Loc = glGetUniformLocation(id, "u_sourceKind1");
  pd.sourceKind2Loc = glGetUniformLocation(id, "u_sourceKind2");
  pd.sourceColor1Loc = glGetUniformLocation(id, "u_sourceColor1");
  pd.sourceColor2Loc = glGetUniformLocation(id, "u_sourceColor2");
  pd.progressLoc = glGetUniformLocation(id, "u_progress");
  pd.fillModeLoc = glGetUniformLocation(id, "u_fillMode");
  pd.imageWidth1Loc = glGetUniformLocation(id, "u_imageWidth1");
  pd.imageHeight1Loc = glGetUniformLocation(id, "u_imageHeight1");
  pd.imageWidth2Loc = glGetUniformLocation(id, "u_imageWidth2");
  pd.imageHeight2Loc = glGetUniformLocation(id, "u_imageHeight2");
  pd.screenWidthLoc = glGetUniformLocation(id, "u_screenWidth");
  pd.screenHeightLoc = glGetUniformLocation(id, "u_screenHeight");
  pd.fillColorLoc = glGetUniformLocation(id, "u_fillColor");
  pd.spanOffsetLoc = glGetUniformLocation(id, "u_spanOffset");
  pd.spanMonitorSizeLoc = glGetUniformLocation(id, "u_spanMonitorSize");
  pd.spanTotalSizeLoc = glGetUniformLocation(id, "u_spanTotalSize");

  // Per-transition (may be -1 if not used by this shader)
  pd.directionLoc = glGetUniformLocation(id, "u_direction");
  pd.smoothnessLoc = glGetUniformLocation(id, "u_smoothness");
  pd.centerXLoc = glGetUniformLocation(id, "u_centerX");
  pd.centerYLoc = glGetUniformLocation(id, "u_centerY");
  pd.aspectRatioLoc = glGetUniformLocation(id, "u_aspectRatio");
  pd.stripeCountLoc = glGetUniformLocation(id, "u_stripeCount");
  pd.angleLoc = glGetUniformLocation(id, "u_angle");
  pd.maxBlockSizeLoc = glGetUniformLocation(id, "u_maxBlockSize");
  pd.cellSizeLoc = glGetUniformLocation(id, "u_cellSize");
  pd.polarity1Loc = glGetUniformLocation(id, "u_polarity1");
  pd.polarity2Loc = glGetUniformLocation(id, "u_polarity2");
  pd.target1Loc = glGetUniformLocation(id, "u_target1");
  pd.target2Loc = glGetUniformLocation(id, "u_target2");
  pd.chromaTableLoc = glGetUniformLocation(id, "u_chromaTable");

  if (pd.positionLoc < 0
      || pd.surfaceSizeLoc < 0
      || pd.quadSizeLoc < 0
      || pd.transformLoc < 0
      || pd.source1Loc < 0
      || pd.progressLoc < 0) {
    throw std::runtime_error("failed to query wallpaper shader locations");
  }
}

void WallpaperProgram::draw(const WallpaperDrawParams& p) const {
  auto idx = static_cast<std::size_t>(p.transition);
  if (idx >= kTransitionCount || !m_programs[idx].program.isValid() || p.quadWidth <= 0.0F || p.quadHeight <= 0.0F) {
    return;
  }
  if (p.from.kind == WallpaperSourceKind::Image && p.from.texture == 0) {
    return;
  }

  const auto& pd = m_programs[idx];
  const TransitionParams& tp = p.params;

  static constexpr float kQuad[] = {
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F,
  };

  glUseProgram(pd.program.id());
  glUniform2f(pd.surfaceSizeLoc, p.surfaceWidth, p.surfaceHeight);
  glUniform2f(pd.quadSizeLoc, p.quadWidth, p.quadHeight);
  glUniformMatrix3fv(pd.transformLoc, 1, GL_FALSE, p.transform.m.data());

  // Textures
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(p.from.texture.value()));
  glUniform1i(pd.source1Loc, 0);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(p.to.texture.value()));
  if (pd.source2Loc >= 0) {
    glUniform1i(pd.source2Loc, 1);
  }

  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, m_chromaTable);
  if (pd.chromaTableLoc >= 0) {
    glUniform1i(pd.chromaTableLoc, 2);
  }
  const auto setPolarity = [](GLint polarityLoc, GLint targetLoc, const wallpaper_polarity::Transform& t) {
    if (polarityLoc >= 0) {
      glUniform4f(polarityLoc, t.invert ? 1.0F : 0.0F, t.sourceLow, t.sourceHigh, 0.0F);
    }
    if (targetLoc >= 0) {
      glUniform2f(targetLoc, t.targetLow, t.targetHigh);
    }
  };
  setPolarity(pd.polarity1Loc, pd.target1Loc, p.from.polarity);
  setPolarity(pd.polarity2Loc, pd.target2Loc, p.to.polarity);

  // Common uniforms
  if (pd.sourceKind1Loc >= 0)
    glUniform1f(pd.sourceKind1Loc, p.from.kind == WallpaperSourceKind::Color ? 1.0F : 0.0F);
  if (pd.sourceKind2Loc >= 0)
    glUniform1f(pd.sourceKind2Loc, p.to.kind == WallpaperSourceKind::Color ? 1.0F : 0.0F);
  if (pd.sourceColor1Loc >= 0)
    glUniform4f(pd.sourceColor1Loc, p.from.color.r, p.from.color.g, p.from.color.b, p.from.color.a);
  if (pd.sourceColor2Loc >= 0)
    glUniform4f(pd.sourceColor2Loc, p.to.color.r, p.to.color.g, p.to.color.b, p.to.color.a);
  glUniform1f(pd.progressLoc, p.progress);
  if (pd.fillModeLoc >= 0)
    glUniform1f(pd.fillModeLoc, p.fillMode);
  if (pd.imageWidth1Loc >= 0)
    glUniform1f(pd.imageWidth1Loc, p.from.imageWidth);
  if (pd.imageHeight1Loc >= 0)
    glUniform1f(pd.imageHeight1Loc, p.from.imageHeight);
  if (pd.imageWidth2Loc >= 0)
    glUniform1f(pd.imageWidth2Loc, p.to.imageWidth);
  if (pd.imageHeight2Loc >= 0)
    glUniform1f(pd.imageHeight2Loc, p.to.imageHeight);
  if (pd.screenWidthLoc >= 0)
    glUniform1f(pd.screenWidthLoc, p.quadWidth);
  if (pd.screenHeightLoc >= 0)
    glUniform1f(pd.screenHeightLoc, p.quadHeight);
  if (pd.fillColorLoc >= 0)
    glUniform4f(pd.fillColorLoc, p.fillColor.r, p.fillColor.g, p.fillColor.b, p.fillColor.a);
  if (pd.spanOffsetLoc >= 0)
    glUniform2f(pd.spanOffsetLoc, p.span.offsetX, p.span.offsetY);
  if (pd.spanMonitorSizeLoc >= 0)
    glUniform2f(pd.spanMonitorSizeLoc, p.span.monitorWidth, p.span.monitorHeight);
  if (pd.spanTotalSizeLoc >= 0)
    glUniform2f(pd.spanTotalSizeLoc, p.span.totalWidth, p.span.totalHeight);

  // Per-transition uniforms
  if (pd.directionLoc >= 0)
    glUniform1f(pd.directionLoc, tp.direction);
  if (pd.smoothnessLoc >= 0)
    glUniform1f(pd.smoothnessLoc, tp.smoothness);
  if (pd.centerXLoc >= 0)
    glUniform1f(pd.centerXLoc, tp.centerX);
  if (pd.centerYLoc >= 0)
    glUniform1f(pd.centerYLoc, tp.centerY);
  if (pd.aspectRatioLoc >= 0)
    glUniform1f(pd.aspectRatioLoc, tp.aspectRatio);
  if (pd.stripeCountLoc >= 0)
    glUniform1f(pd.stripeCountLoc, tp.stripeCount);
  if (pd.angleLoc >= 0)
    glUniform1f(pd.angleLoc, tp.angle);
  if (pd.maxBlockSizeLoc >= 0)
    glUniform1f(pd.maxBlockSizeLoc, tp.maxBlockSize);
  if (pd.cellSizeLoc >= 0)
    glUniform1f(pd.cellSizeLoc, tp.cellSize);

  // Draw fullscreen quad
  auto posAttr = static_cast<GLuint>(pd.positionLoc);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);

  glActiveTexture(GL_TEXTURE0);
}
