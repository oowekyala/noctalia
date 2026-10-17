#include "theme/palette_transform.h"

#include "config/config_types.h"
#include "cpp/cam/hct.h"
#include "theme/color.h"
#include "theme/contrast.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
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

    // The six terminal colors, each with its own hue (OKLCH degrees) and, with PinnedHues, the
    // most it may turn towards its target (see harmonyTargets).
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
      // The chroma of red and green, at least: they must stay apart (diffs), also with
      // the schemes whose own chroma is lower than that. Monochrome is the exception: it
      // is for people who want no color, so there they follow its chroma.
      double distinctChroma;
    };

    [[nodiscard]] constexpr PinnedPreset pinnedPreset(Scheme base) {
      switch (base) {
      case Scheme::FruitSalad:
        return {0.16, 1.3, 0.10};
      case Scheme::Monochrome:
        return {0.05, 0.5, 0.05};
      case Scheme::Vibrant:
        return {0.18, 1.0, 0.10};
      case Scheme::Faithful:
        return {0.16, 1.0, 0.10};
      case Scheme::Soft:
        return {0.10, 0.8, 0.10};
      case Scheme::Dysfunctional:
        return {0.16, 2.0, 0.10};
      case Scheme::Muted:
        return {0.07, 0.5, 0.10};
      case Scheme::TonalSpot:
      case Scheme::Content:
      case Scheme::Rainbow:
        break;
      }
      return {0.15, 1.0, 0.10};
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

    [[nodiscard]] double wrapDegrees(double degrees) {
      degrees = std::fmod(degrees, 360.0);
      return degrees < 0.0 ? degrees + 360.0 : degrees;
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

    // Where the colors go: one dominant hue family, as most hand-made color schemes have. Four
    // colors head for hues around the seed's, two for hues around its complement; exactly one of
    // red and green is among those two, so that they stay apart (diffs). Within the family, chroma
    // and contrast alternate, so that neighbouring hues differ in strength and lightness too.
    struct HarmonyTarget {
      double offset; // from the seed's hue, in degrees
      double chromaFactor = 1.0;
      double contrastFactor = 1.0;
    };
    constexpr std::array<HarmonyTarget, 6> kHarmonyTargets = {{
        {-36.0, 1.0, 1.0},
        {-12.0, 0.7, 1.3},
        {12.0, 1.15, 1.0},
        {36.0, 0.8, 1.3},
        {162.0},
        {198.0},
    }};
    constexpr std::size_t kFirstComplementTarget = 4;
    constexpr std::size_t kRedSlot = 0;
    constexpr std::size_t kGreenSlot = 2;

    [[nodiscard]] double signedArc(double from, double to) { return wrapDegrees(to - from + 180.0) - 180.0; }

    // Which target (index into kHarmonyTargets) each color (index into kPinnedHues) heads for: of the
    // assignments that keep red and green apart, the one that moves the colors the least from their
    // own hues (on a red wallpaper red stays in the family and green goes to the complement).
    [[nodiscard]] std::array<std::size_t, 6> harmonyTargets(double seedHue) {
      std::array<std::size_t, 6> order{0, 1, 2, 3, 4, 5};
      std::array<std::size_t, 6> best = order;
      double bestCost = std::numeric_limits<double>::max();
      do {
        if ((order[kRedSlot] >= kFirstComplementTarget) == (order[kGreenSlot] >= kFirstComplementTarget)) {
          continue;
        }
        double cost = 0.0;
        for (std::size_t i = 0; i < order.size(); ++i) {
          cost += std::abs(signedArc(kPinnedHues[i].hue, seedHue + kHarmonyTargets[order[i]].offset));
        }
        if (cost < bestCost) {
          bestCost = cost;
          best = order;
        }
      } while (std::next_permutation(order.begin(), order.end()));
      return best;
    }

    // PinnedHues and Balanced are one algorithm at two strengths: each color moves from its own hue
    // towards its target, half the way and at most its cap (PinnedHues), or all the way and with the
    // target's chroma and contrast (Balanced); either way less so with a nearly gray seed. Then its
    // lightness is searched for the contrast target on the background.
    void applyTerminalColors(TokenMap& tokens, TerminalScheme terminal, Scheme base, std::uint32_t seedArgb) {
      const auto backgroundIt = tokens.find("terminal_background");
      if (backgroundIt == tokens.end()) {
        return;
      }
      const Color background = Color::fromArgb(backgroundIt->second);
      const Oklch seed = toOklch(Color::fromArgb(seedArgb));
      const double seedHue = wrapDegrees(seed.hueRadians * 180.0 / std::numbers::pi);
      const double pull = std::min(1.0, seed.chroma / kSeedFullChroma);
      const bool balanced = terminal == TerminalScheme::Balanced;
      const std::array<std::size_t, 6> targets = harmonyTargets(seedHue);
      const PinnedPreset preset = pinnedPreset(base);
      const bool dark = isDark(background);
      const double target = dark ? kPinnedContrastDark : kPinnedContrastLight;
      const double step = dark ? kBrightLightnessStep : -kBrightLightnessStep;

      for (std::size_t i = 0; i < kPinnedHues.size(); ++i) {
        const PinnedHue& pinned = kPinnedHues[i];
        const HarmonyTarget goal =
            balanced ? kHarmonyTargets[targets[i]] : HarmonyTarget{kHarmonyTargets[targets[i]].offset};
        double move = signedArc(pinned.hue, seedHue + goal.offset);
        if (!balanced) {
          const double cap = pinned.cap * preset.capScale;
          move = std::clamp(move / 2.0, -cap, cap);
        }
        const double hueRadians = wrapDegrees(pinned.hue + move * pull) * std::numbers::pi / 180.0;
        double chroma = preset.chroma * goal.chromaFactor;
        if (pinned.keepDistinct) {
          chroma = std::max(chroma, preset.distinctChroma);
        }
        const Color normal = colorForContrast(chroma, hueRadians, background, target * goal.contrastFactor);
        const Oklch normalLch = toOklch(normal);
        const Color bright =
            gamutMapToSrgb({std::clamp(normalLch.lightness + step, 0.0, 1.0), chroma * kBrightChromaRatio, hueRadians});
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

  void applyTerminalScheme(GeneratedPalette& palette, TerminalScheme terminal, Scheme base) {
    if (terminal == TerminalScheme::Default) {
      return;
    }
    for (TokenMap* tokens : {&palette.dark, &palette.light}) {
      // The wallpaper's accent, or the source color of a palette without one (one loaded from JSON).
      std::optional<std::uint32_t> seed = palette.accent;
      if (const auto source = tokens->find("source_color"); !seed.has_value() && source != tokens->end()) {
        seed = source->second;
      }
      if (seed.has_value() && !tokens->empty()) {
        applyTerminalColors(*tokens, terminal, base, *seed);
      }
    }
  }

} // namespace noctalia::theme
