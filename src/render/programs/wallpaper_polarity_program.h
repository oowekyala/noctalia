#pragma once

#include "render/core/shader_program.h"
#include "render/core/wallpaper_polarity.h"

#include <GLES2/gl2.h>
#include <cstdint>
#include <vector>

// The wallpaper polarity pass (render/core/wallpaper_polarity.h): renders an image with its
// lightness inverted, in OKLab, into a texture of the same size. Hues stay; the image's
// lightness range is stretched, reversed, onto the target range, compared in Okhsl's toe
// lightness (OKLab overstates near-black tones, so a compressed black background's noise
// would show once white) above a small black point; near-black pixels lose their chroma;
// and colors keep the chroma they had where sRGB can show it at the new lightness, else
// the most it can show there, in the same hue.
//
// Runs once per image, so it can afford finding that chroma by bisection per pixel.
class WallpaperPolarityProgram {
public:
  WallpaperPolarityProgram() = default;
  ~WallpaperPolarityProgram() = default;

  WallpaperPolarityProgram(const WallpaperPolarityProgram&) = delete;
  WallpaperPolarityProgram& operator=(const WallpaperPolarityProgram&) = delete;

  void ensureInitialized();
  void destroy();
  void abandon() noexcept;

  // Renders `source` (width x height, RGBA) into `target` (an RGBA texture of the same
  // size), in the GL context current on this thread, which it leaves as it found it. With
  // `readback`, also reads the result back (RGBA, rows top to bottom, as uploaded). False
  // when the framebuffer can't be set up.
  bool render(
      GLuint source, GLuint target, int width, int height, const wallpaper_polarity::Analysis& analysis,
      std::vector<std::uint8_t>* readback
  );

private:
  ShaderProgram m_program;
  GLint m_positionLoc = -1;
  GLint m_sourceLoc = -1;
  GLint m_sourceRangeLoc = -1;
  GLint m_targetRangeLoc = -1;
};
