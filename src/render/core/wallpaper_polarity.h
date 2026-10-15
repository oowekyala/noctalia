#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Wallpaper polarity: a wallpaper that doesn't suit the theme mode (a dark image in
// light mode, or the reverse) is shown with its lightness inverted, in OKLab, so its
// hues stay (a purple stays purple, unlike a negative). Lightness is stretched from
// the image's own range onto a range suited to the mode, darkest tones becoming the
// lightest and the reverse; chroma is kept where sRGB can show it at the new
// lightness, and reduced only as far as needed where it can't (vivid deep blues have
// no light counterpart of the same saturation). Lightness is compared in toe lightness, with
// a small black point, and near-black pixels lose their chroma, so the noise of a compressed
// black background doesn't turn into visible blocks once it is white.
//
// The wallpaper shader does the inversion on screen; the theme service does it on the
// small buffer it generates the palette from, so the palette matches what is shown.
namespace wallpaper_polarity {

  // The overall lightness of an image, measured once per file.
  struct Analysis {
    bool light = false;    // mean OKLab lightness above 0.5
    float darkest = 0.0F;  // 2nd percentile of the lightness
    float lightest = 1.0F; // 98th percentile

    bool operator==(const Analysis&) const = default;
  };

  // How a layer is drawn: inverted or not, and if so from which range onto which. The ranges are in
  // toe lightness (toe() below), the source one already raised by the black point.
  struct Transform {
    bool invert = false;
    float sourceLow = 0.0F;
    float sourceHigh = 1.0F;
    float targetLow = 0.0F;
    float targetHigh = 1.0F;

    bool operator==(const Transform&) const = default;
  };

  // Okhsl's toe: OKLab lightness mapped closer to CIELAB's, which OKLab overstates near black (sRGB 1/255
  // already has lightness 0.07). The inversion runs on it, so the compression noise of a "black" background
  // stays invisible once that background is white.
  [[nodiscard]] float toe(float lightness);
  [[nodiscard]] float toeInverse(float lightness);

  // Analyzes an 8-bit image (3 or 4 channels, sRGB) from an evenly spaced sample of at
  // most 256x256 of its pixels.
  [[nodiscard]] Analysis analyze(const std::uint8_t* pixels, int width, int height, int channels);

  // The transform for an image in a theme mode: inverted when its polarity doesn't
  // match the mode, onto the lightness range for that mode.
  [[nodiscard]] Transform transformFor(const Analysis& analysis, bool lightMode);

  // Inverts 8-bit sRGB pixels in place (3 or 4 channels; alpha is kept). Does nothing
  // for a transform that doesn't invert.
  void apply(std::uint8_t* pixels, std::size_t pixelCount, int channels, const Transform& transform);

  // The largest chroma sRGB can show, per (lightness, hue) step: kTableLightness rows
  // of kTableHues columns, lightness 0 to 1 and hue 0 to 359 degrees.
  inline constexpr int kTableLightness = 256;
  inline constexpr int kTableHues = 360;
  // Chroma never reaches this inside sRGB; the shader's 8-bit copy of the table is
  // stored as a fraction of it.
  inline constexpr float kTableMaxChroma = 0.4F;
  [[nodiscard]] const std::vector<float>& maxChromaTable();

  // Analyses by image path, shared by everything that shows or reads a wallpaper so they
  // agree on whether it is inverted. The first analysis stored for a path wins; a
  // changed file (other modification time) is analyzed again.
  [[nodiscard]] std::optional<Analysis> cached(const std::string& path);
  Analysis remember(const std::string& path, const Analysis& analysis);

} // namespace wallpaper_polarity
