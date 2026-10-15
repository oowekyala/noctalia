#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Wallpaper polarity: a wallpaper that doesn't suit the theme mode (a dark image in
// light mode, or the reverse) is shown with its lightness inverted, in OKLab, so its
// hues stay (a purple stays purple, unlike a negative).
//
// The inversion itself is a GPU pass (render/programs/wallpaper_polarity_program.h),
// run once per image by the shared texture cache: the inverted image is a texture of
// its own, which the wallpaper crossfades to like to any other image. The pass's
// output is also read back once, for what needs pixels or a file: the palette (a
// small preview), the greeter and thumbnails (a PNG in the cache directory).
//
// This side only measures images, and keeps what the pass produced.
namespace wallpaper_polarity {

  // The overall lightness of an image, measured once per file.
  struct Analysis {
    bool light = false;    // mean OKLab lightness above 0.5
    float darkest = 0.0F;  // 2nd percentile of the lightness
    float lightest = 1.0F; // 98th percentile

    bool operator==(const Analysis&) const = default;
  };

  // Analyzes an 8-bit image (3 or 4 channels, sRGB) from an evenly spaced sample of at
  // most 256x256 of its pixels.
  [[nodiscard]] Analysis analyze(const std::uint8_t* pixels, int width, int height, int channels);

  // Whether an image is shown inverted in a theme mode: when its polarity doesn't match.
  [[nodiscard]] constexpr bool invertedIn(const Analysis& analysis, bool lightMode) {
    return analysis.light != lightMode;
  }

  // What the pass maps an image onto: the OKLab lightness range of the mode it is
  // inverted for (a light wallpaper stays bright enough behind light surfaces, a dark one
  // dark). An image is only ever inverted for the mode opposite to its own polarity.
  struct TargetRange {
    float low;
    float high;
  };
  [[nodiscard]] TargetRange targetRangeFor(const Analysis& analysis);

  // Analyses by image path, shared by everything that shows or reads a wallpaper so they
  // agree on whether it is inverted. The first analysis stored for a path wins; a
  // changed file (other modification time) is analyzed again.
  [[nodiscard]] std::optional<Analysis> cached(const std::string& path);
  Analysis remember(const std::string& path, const Analysis& analysis);

  // The inverted version of an image, from the pass's read-back pixels (RGBA, rows top to
  // bottom). Keeps a small preview in memory, writes a PNG in the background, and tells
  // the listeners. Main thread.
  void storeVariant(const std::string& path, const std::vector<std::uint8_t>& rgba, int width, int height);

  struct Preview {
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
  };
  // The preview of an image's inverted version, at most 512 pixels wide, if this session
  // rendered it. Thread-safe.
  [[nodiscard]] std::optional<Preview> variantPreview(const std::string& path);

  // The PNG of an image's inverted version, once written (this session or an earlier one,
  // for the same file contents).
  [[nodiscard]] std::optional<std::string> variantFile(const std::string& path);

  // Called on the main thread with the image path, whenever a variant is stored.
  using Listener = std::function<void(const std::string& path)>;
  [[nodiscard]] std::uint64_t addVariantListener(Listener listener);
  void removeVariantListener(std::uint64_t id);

} // namespace wallpaper_polarity
