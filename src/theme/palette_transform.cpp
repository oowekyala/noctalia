#include "theme/palette_transform.h"

#include "cpp/cam/hct.h"
#include "theme/color.h"
#include "theme/contrast.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace noctalia::theme {

  namespace {

    namespace mcu = material_color_utilities;

    // The dark elevation ladder, plus the two terminal tokens that are the background
    // itself. terminal_selection_bg and terminal_normal_black are deliberately absent:
    // a terminal needs them to stay visible against the background, not to follow it.
    constexpr std::array<std::string_view, 12> kSurfaceRamp = {{
        "background",
        "surface",
        "surface_variant",
        "surface_dim",
        "surface_bright",
        "surface_container_lowest",
        "surface_container_low",
        "surface_container",
        "surface_container_high",
        "surface_container_highest",
        "terminal_background",
        "terminal_cursor_text",
    }};

    [[nodiscard]] std::uint32_t lowerTone(std::uint32_t argb, double shift) {
      mcu::Hct hct(argb);
      hct.set_tone(std::max(0.0, hct.get_tone() - shift));
      return hct.ToInt();
    }

    // TerminalScheme::PinnedHues. Each color has a fixed hue (OKLCH degrees) and may be
    // rotated towards the seed's hue by half their difference, at most `cap` degrees.
    struct PinnedHue {
      std::string_view name;
      double hue;
      double cap;
      bool keepDistinct; // red and green: diffs, test output
    };
    constexpr std::array<PinnedHue, 6> kPinnedHues = {{
        {"red", 29.0, 15.0, true},
        {"yellow", 95.0, 15.0, false},
        {"green", 145.0, 15.0, true},
        {"cyan", 200.0, 25.0, false},
        {"blue", 260.0, 25.0, false},
        {"magenta", 330.0, 25.0, false},
    }};

    // What a base scheme's character asks of the terminal colors: the chroma of the normal
    // colors, and a factor on the rotation caps.
    struct PinnedPreset {
      double chroma;
      double capScale;
    };

    [[nodiscard]] constexpr PinnedPreset pinnedPreset(Scheme base) {
      switch (base) {
      case Scheme::FruitSalad:
        return {0.16, 1.3};
      case Scheme::Monochrome:
        return {0.05, 0.5};
      case Scheme::Vibrant:
        return {0.18, 1.0};
      case Scheme::Faithful:
        return {0.16, 1.0};
      case Scheme::Soft:
        return {0.10, 0.8};
      case Scheme::Dysfunctional:
        return {0.16, 2.0};
      case Scheme::Muted:
        return {0.07, 0.5};
      case Scheme::TonalSpot:
      case Scheme::Content:
      case Scheme::Rainbow:
        break;
      }
      return {0.15, 1.0};
    }

    // Contrast of the normal colors on the terminal background.
    constexpr double kPinnedContrastDark = 6.0;
    constexpr double kPinnedContrastLight = 5.0;
    // Bright colors sit this far further from the background in lightness (OKLCH), with
    // their chroma at this fraction of the normal one.
    constexpr double kBrightLightnessStep = 0.08;
    constexpr double kBrightChromaRatio = 0.8;
    // Below this chroma the seed is nearly gray and its hue says little; the rotation is
    // scaled down to nothing as the chroma approaches zero.
    constexpr double kSeedFullChroma = 0.08;
    // Red and green keep at least this chroma, whatever the base scheme, so that they stay
    // apart (diffs). Low enough for Monochrome and Muted to stay clearly more muted than the
    // other schemes: they are the base scheme's own chroma, which is what they are for.
    constexpr double kDistinctMinChroma = 0.04;
    constexpr double kPi = 3.14159265358979323846;

    [[nodiscard]] double wrapDegrees(double degrees) {
      degrees = std::fmod(degrees, 360.0);
      return degrees < 0.0 ? degrees + 360.0 : degrees;
    }

    // `hue` rotated towards `seedHue` by half their difference, at most `cap`, scaled by `pull`.
    [[nodiscard]] double harmonizedHue(double hue, double seedHue, double cap, double pull) {
      const double diff = wrapDegrees(seedHue - hue + 180.0) - 180.0;
      return wrapDegrees(hue + std::clamp(diff / 2.0, -cap, cap) * pull);
    }

    // The color of (chroma, hue) whose lightness is the closest to the background's at which
    // it reaches `target` contrast: lighter on a dark background, darker on a light one.
    [[nodiscard]] Color colorForContrast(double chroma, double hueRadians, const Color& background, double target) {
      const double step = isDark(background) ? 0.005 : -0.005;
      double lightness = toOklch(background).lightness;
      Color color = gamutMapToSrgb({lightness, chroma, hueRadians});
      while (lightness + step >= 0.0 && lightness + step <= 1.0) {
        lightness += step;
        color = gamutMapToSrgb({lightness, chroma, hueRadians});
        if (contrastRatio(color, background) >= target) {
          break;
        }
      }
      return color;
    }

    void applyPinnedHues(TokenMap& tokens, Scheme base) {
      const auto sourceIt = tokens.find("source_color");
      const auto backgroundIt = tokens.find("terminal_background");
      if (sourceIt == tokens.end() || backgroundIt == tokens.end()) {
        return;
      }
      const Color background = Color::fromArgb(backgroundIt->second);
      const Oklch seed = toOklch(Color::fromArgb(sourceIt->second));
      const double seedHue = wrapDegrees(seed.hueRadians * 180.0 / kPi);
      const double pull = std::min(1.0, seed.chroma / kSeedFullChroma);
      const PinnedPreset preset = pinnedPreset(base);
      const bool dark = isDark(background);
      const double target = dark ? kPinnedContrastDark : kPinnedContrastLight;
      const double step = dark ? kBrightLightnessStep : -kBrightLightnessStep;

      for (const PinnedHue& pinned : kPinnedHues) {
        const double hueRadians =
            harmonizedHue(pinned.hue, seedHue, pinned.cap * preset.capScale, pull) * kPi / 180.0;
        const double chroma = pinned.keepDistinct ? std::max(preset.chroma, kDistinctMinChroma) : preset.chroma;
        const Color normal = colorForContrast(chroma, hueRadians, background, target);
        const Oklch normalLch = toOklch(normal);
        const Color bright = gamutMapToSrgb({std::clamp(normalLch.lightness + step, 0.0, 1.0),
                                             chroma * kBrightChromaRatio, hueRadians});
        tokens["terminal_normal_" + std::string(pinned.name)] = normal.toArgb();
        tokens["terminal_bright_" + std::string(pinned.name)] = bright.toArgb();
      }
    }

  } // namespace

  void applyPureBlackDark(TokenMap& darkTokens) {
    const auto base = darkTokens.find("surface");
    if (base == darkTokens.end()) {
      return;
    }
    // Shifting by the base tone puts `surface` on tone 0 and drops everything above it
    // by the same amount; tokens already below the base clamp to black.
    const double shift = mcu::Hct(base->second).get_tone();
    if (shift <= 0.0) {
      return;
    }
    for (const std::string_view key : kSurfaceRamp) {
      if (const auto it = darkTokens.find(std::string(key)); it != darkTokens.end()) {
        it->second = lowerTone(it->second, shift);
      }
    }
  }

  void applyPureBlackDark(GeneratedPalette& palette) {
    if (!palette.dark.empty()) {
      applyPureBlackDark(palette.dark);
    }
  }

  void applyHighContrast(TokenMap& tokens, bool isDark) {
    for (auto& [key, value] : tokens) {
      mcu::Hct hct(value);
      double tone = hct.get_tone();

      if (key == "outline" || key == "outline_variant") {
        // Force borders to be highly visible: extremely light in dark mode, extremely dark in light mode
        tone = isDark ? std::max(tone, 80.0) : std::min(tone, 20.0);
      } else {
        // Aggressive contrast stretch for everything else:
        // Push darks darker and lights lighter to separate backgrounds from foregrounds
        if (tone < 50.0) {
          tone = std::max(0.0, tone - 20.0);
        } else {
          tone = std::min(100.0, tone + 20.0);
        }
      }

      hct.set_tone(tone);
      value = hct.ToInt();
    }
  }

  void applyHighContrast(GeneratedPalette& palette) {
    if (!palette.dark.empty()) {
      applyHighContrast(palette.dark, true);
    }
    if (!palette.light.empty()) {
      applyHighContrast(palette.light, false);
    }
  }

  void applyTerminalScheme(TokenMap& tokens, TerminalScheme terminal, Scheme base) {
    switch (terminal) {
    case TerminalScheme::Default:
      return;
    case TerminalScheme::PinnedHues:
      applyPinnedHues(tokens, base);
      return;
    }
  }

  void applyTerminalScheme(GeneratedPalette& palette, TerminalScheme terminal, Scheme base) {
    if (!palette.dark.empty()) {
      applyTerminalScheme(palette.dark, terminal, base);
    }
    if (!palette.light.empty()) {
      applyTerminalScheme(palette.light, terminal, base);
    }
  }

} // namespace noctalia::theme
