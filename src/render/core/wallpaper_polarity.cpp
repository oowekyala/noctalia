#include "render/core/wallpaper_polarity.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <numbers>
#include <unordered_map>

namespace wallpaper_polarity {

  namespace {

    // Lightness range the inverted image is mapped into, per mode: a light wallpaper
    // stays bright enough behind light surfaces, a dark one dark.
    constexpr float kLightLow = 0.42F;
    constexpr float kLightHigh = 0.98F;
    constexpr float kDarkLow = 0.10F;
    constexpr float kDarkHigh = 0.72F;
    // Mean lightness above which an image counts as light.
    constexpr float kLightThreshold = 0.5F;
    constexpr int kSampleSide = 256;
    // In toe lightness: tones this close to the image's darkest count as its darkest (about 4/255 of sRGB
    // noise over black).
    constexpr float kBlackPoint = 0.03F;
    // In toe lightness: below this, chroma fades out (it can't be seen there, and in a compressed image it is
    // mostly noise).
    constexpr float kChromaFloor = 0.1F;
    // Okhsl's toe constants.
    constexpr float kToeK1 = 0.206F;
    constexpr float kToeK2 = 0.03F;
    constexpr float kToeK3 = (1.0F + kToeK1) / (1.0F + kToeK2);

    struct Lab {
      double L;
      double a;
      double b;
    };

    struct LinearRgb {
      double r;
      double g;
      double b;
    };

    double decodeChannel(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }

    double encodeChannel(double c) {
      c = std::clamp(c, 0.0, 1.0);
      return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    }

    const std::array<double, 256>& decodeTable() {
      static const std::array<double, 256> table = [] {
        std::array<double, 256> t{};
        for (int i = 0; i < 256; ++i) {
          t[static_cast<std::size_t>(i)] = decodeChannel(i / 255.0);
        }
        return t;
      }();
      return table;
    }

    Lab toLab(const LinearRgb& c) {
      const double l = std::cbrt(0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b);
      const double m = std::cbrt(0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b);
      const double s = std::cbrt(0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b);
      return {
          .L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
          .a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
          .b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
      };
    }

    LinearRgb toLinear(const Lab& c) {
      const double l = std::pow(c.L + 0.3963377774 * c.a + 0.2158037573 * c.b, 3.0);
      const double m = std::pow(c.L - 0.1055613458 * c.a - 0.0638541728 * c.b, 3.0);
      const double s = std::pow(c.L - 0.0894841775 * c.a - 1.2914855480 * c.b, 3.0);
      return {
          .r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
          .g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
          .b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
      };
    }

    Lab pixelLab(const std::uint8_t* p) {
      const auto& decode = decodeTable();
      return toLab({decode[p[0]], decode[p[1]], decode[p[2]]});
    }

    bool inGamut(const LinearRgb& c) {
      constexpr double tolerance = 1e-4;
      return c.r >= -tolerance
          && c.r <= 1.0 + tolerance
          && c.g >= -tolerance
          && c.g <= 1.0 + tolerance
          && c.b >= -tolerance
          && c.b <= 1.0 + tolerance;
    }

    float percentile(std::vector<float>& values, double fraction) {
      const auto index = static_cast<std::size_t>(std::lround(fraction * static_cast<double>(values.size() - 1)));
      std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
      return values[index];
    }

    struct CacheEntry {
      Analysis analysis;
      std::filesystem::file_time_type modified;
    };

    std::filesystem::file_time_type modifiedTime(const std::string& path) {
      std::error_code ec;
      const auto time = std::filesystem::last_write_time(path, ec);
      return ec ? std::filesystem::file_time_type{} : time;
    }

    std::mutex& cacheMutex() {
      static std::mutex mutex;
      return mutex;
    }

    std::unordered_map<std::string, CacheEntry>& cache() {
      static std::unordered_map<std::string, CacheEntry> entries;
      return entries;
    }

  } // namespace

  Analysis analyze(const std::uint8_t* pixels, int width, int height, int channels) {
    if (pixels == nullptr || width <= 0 || height <= 0 || channels < 3) {
      return {};
    }
    const int stepX = std::max(1, width / kSampleSide);
    const int stepY = std::max(1, height / kSampleSide);
    std::vector<float> lightness;
    lightness.reserve(static_cast<std::size_t>((width / stepX + 1) * (height / stepY + 1)));
    double sum = 0.0;
    for (int y = 0; y < height; y += stepY) {
      const std::uint8_t* row =
          pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
      for (int x = 0; x < width; x += stepX) {
        const float L =
            static_cast<float>(pixelLab(row + static_cast<std::size_t>(x) * static_cast<std::size_t>(channels)).L);
        lightness.push_back(L);
        sum += L;
      }
    }
    const double mean = sum / static_cast<double>(lightness.size());
    const float darkest = percentile(lightness, 0.02);
    const float lightest = percentile(lightness, 0.98);
    return {.light = mean > kLightThreshold, .darkest = darkest, .lightest = lightest};
  }

  float toe(float lightness) {
    const float x = std::max(lightness, 0.0F);
    const float a = kToeK3 * x - kToeK1;
    return 0.5F * (a + std::sqrt(a * a + 4.0F * kToeK2 * kToeK3 * x));
  }

  float toeInverse(float lightness) {
    return (lightness * lightness + kToeK1 * lightness) / (kToeK3 * (lightness + kToeK2));
  }

  Transform transformFor(const Analysis& analysis, bool lightMode) {
    if (analysis.light == lightMode) {
      return {};
    }
    const float sourceLow = toe(analysis.darkest) + kBlackPoint;
    return {
        .invert = true,
        .sourceLow = sourceLow,
        .sourceHigh = std::max(toe(analysis.lightest), sourceLow + 1e-3F),
        .targetLow = toe(lightMode ? kLightLow : kDarkLow),
        .targetHigh = toe(lightMode ? kLightHigh : kDarkHigh),
    };
  }

  const std::vector<float>& maxChromaTable() {
    static const std::vector<float> table = [] {
      std::vector<float> lo(static_cast<std::size_t>(kTableLightness * kTableHues), 0.0F);
      for (int i = 0; i < kTableLightness; ++i) {
        const double L = static_cast<double>(i) / (kTableLightness - 1);
        for (int h = 0; h < kTableHues; ++h) {
          const double hue = h * std::numbers::pi / 180.0;
          double low = 0.0;
          double high = kTableMaxChroma;
          for (int step = 0; step < 20; ++step) {
            const double c = (low + high) / 2.0;
            if (inGamut(toLinear({L, c * std::cos(hue), c * std::sin(hue)}))) {
              low = c;
            } else {
              high = c;
            }
          }
          lo[static_cast<std::size_t>(i * kTableHues + h)] = static_cast<float>(low);
        }
      }
      // Rounding to the nearest step could overshoot a little at a curved boundary: take the smallest of each
      // entry and its neighbours one lightness step up (the last row wraps to the first, as numpy.roll does in the
      // script this is ported from), at the same hue and one hue step down.
      std::vector<float> out(lo.size());
      const auto at = [&](int row, int col) { return lo[static_cast<std::size_t>(row * kTableHues + col)]; };
      for (int i = 0; i < kTableLightness; ++i) {
        const int up = (i + 1) % kTableLightness;
        for (int h = 0; h < kTableHues; ++h) {
          const int previous = (h + kTableHues - 1) % kTableHues;
          out[static_cast<std::size_t>(i * kTableHues + h)] = std::min({at(i, h), at(up, h), at(up, previous)});
        }
      }
      return out;
    }();
    return table;
  }

  void apply(std::uint8_t* pixels, std::size_t pixelCount, int channels, const Transform& transform) {
    if (!transform.invert || pixels == nullptr || channels < 3) {
      return;
    }
    const auto& table = maxChromaTable();
    const float range = std::max(transform.sourceHigh - transform.sourceLow, 1e-3F);
    for (std::size_t i = 0; i < pixelCount; ++i) {
      std::uint8_t* p = pixels + i * static_cast<std::size_t>(channels);
      Lab lab = pixelLab(p);
      const float sourceToe = toe(static_cast<float>(lab.L));
      const double t = std::clamp((sourceToe - transform.sourceLow) / range, 0.0F, 1.0F);
      const double L =
          toeInverse(static_cast<float>(transform.targetHigh - (transform.targetHigh - transform.targetLow) * t));
      const float fade = std::clamp(sourceToe / kChromaFloor, 0.0F, 1.0F);
      const double chromaKept = fade * fade * (3.0F - 2.0F * fade);
      lab.a *= chromaKept;
      lab.b *= chromaKept;
      const double chroma = std::hypot(lab.a, lab.b);
      double hue = std::atan2(lab.b, lab.a) * 180.0 / std::numbers::pi;
      if (hue < 0.0) {
        hue += 360.0;
      }
      const auto row = static_cast<int>(std::lround(L * (kTableLightness - 1)));
      const int col = static_cast<int>(std::lround(hue)) % kTableHues;
      const double maxChroma = table[static_cast<std::size_t>(row * kTableHues + col)];
      const double scale = chroma > maxChroma ? maxChroma / std::max(chroma, 1e-9) : 1.0;
      const LinearRgb out = toLinear({L, lab.a * scale, lab.b * scale});
      p[0] = static_cast<std::uint8_t>(std::lround(encodeChannel(out.r) * 255.0));
      p[1] = static_cast<std::uint8_t>(std::lround(encodeChannel(out.g) * 255.0));
      p[2] = static_cast<std::uint8_t>(std::lround(encodeChannel(out.b) * 255.0));
    }
  }

  std::optional<Analysis> cached(const std::string& path) {
    const auto modified = modifiedTime(path);
    const std::lock_guard lock(cacheMutex());
    const auto it = cache().find(path);
    if (it == cache().end() || it->second.modified != modified) {
      return std::nullopt;
    }
    return it->second.analysis;
  }

  Analysis remember(const std::string& path, const Analysis& analysis) {
    const auto modified = modifiedTime(path);
    const std::lock_guard lock(cacheMutex());
    auto [it, inserted] = cache().try_emplace(path, CacheEntry{analysis, modified});
    if (!inserted && it->second.modified != modified) {
      it->second = CacheEntry{analysis, modified};
    }
    return it->second.analysis;
  }

} // namespace wallpaper_polarity
