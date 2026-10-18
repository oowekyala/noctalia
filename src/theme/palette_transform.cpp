#include "theme/palette_transform.h"

#include "config/config_types.h"
#include "cpp/cam/hct.h"
#include "theme/color.h"
#include "theme/contrast.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

    // TerminalScheme::PinnedHues. Each color has a fixed hue (OKLCH degrees), in hue order, and may
    // be rotated towards the seed's hue by half their difference, at most `cap` degrees.
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
    // Neighbouring colors stay at least this far apart in hue (degrees), however far the scheme
    // lets them rotate: each turns at most halfway into the gap between their pinned hues, less
    // half of this.
    constexpr double kMinHueGap = 30.0;
    // Below this chroma the seed is nearly gray and its hue says little; the rotation is
    // scaled down to nothing as the chroma approaches zero.
    constexpr double kSeedFullChroma = 0.08;

    [[nodiscard]] double wrapDegrees(double degrees) {
      degrees = std::fmod(degrees, 360.0);
      return degrees < 0.0 ? degrees + 360.0 : degrees;
    }

    // The pinned hue of kPinnedHues[index] rotated towards `seedHue` by half their difference, at
    // most `cap` and no closer to its neighbours than kMinHueGap allows, scaled by `pull`.
    [[nodiscard]] double harmonizedHue(std::size_t index, double seedHue, double cap, double pull) {
      const double hue = kPinnedHues[index].hue;
      const double previous = kPinnedHues[(index + kPinnedHues.size() - 1) % kPinnedHues.size()].hue;
      const double next = kPinnedHues[(index + 1) % kPinnedHues.size()].hue;
      const double down = std::min(cap, (wrapDegrees(hue - previous) - kMinHueGap) / 2.0);
      const double up = std::min(cap, (wrapDegrees(next - hue) - kMinHueGap) / 2.0);
      const double diff = wrapDegrees(seedHue - hue + 180.0) - 180.0;
      return wrapDegrees(hue + std::clamp(diff / 2.0, -down, up) * pull);
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
      const double seedHue = wrapDegrees(seed.hueRadians * 180.0 / std::numbers::pi);
      const double pull = std::min(1.0, seed.chroma / kSeedFullChroma);
      const PinnedPreset preset = pinnedPreset(base);
      const bool dark = isDark(background);
      const double target = dark ? kPinnedContrastDark : kPinnedContrastLight;
      const double step = dark ? kBrightLightnessStep : -kBrightLightnessStep;

      for (std::size_t i = 0; i < kPinnedHues.size(); ++i) {
        const PinnedHue& pinned = kPinnedHues[i];
        const double hueRadians =
            harmonizedHue(i, seedHue, pinned.cap * preset.capScale, pull) * std::numbers::pi / 180.0;
        const double chroma = pinned.keepDistinct ? std::max(preset.chroma, preset.distinctChroma) : preset.chroma;
        const Color normal = colorForContrast(chroma, hueRadians, background, target);
        const Oklch normalLch = toOklch(normal);
        const Color bright =
            gamutMapToSrgb({std::clamp(normalLch.lightness + step, 0.0, 1.0), chroma * kBrightChromaRatio, hueRadians});
        tokens["terminal_normal_" + std::string(pinned.name)] = normal.toArgb();
        tokens["terminal_bright_" + std::string(pinned.name)] = bright.toArgb();
      }

      // Black and white the usual way round in both modes, as in most hand-made terminal themes:
      // black dark, white light, and each bright one lighter. Dark mode: black is near the
      // background (status lines use it as one) and white is text. Light mode: black is text and
      // white near the background, so white text is faint there.
      const auto role = [&](const char* key) -> std::optional<Color> {
        const auto it = tokens.find(key);
        return it == tokens.end() ? std::nullopt : std::optional(Color::fromArgb(it->second));
      };
      const auto set = [&](const char* key, std::optional<Color> color) {
        if (color.has_value()) {
          tokens[key] = color->toArgb();
        }
      };
      const std::optional<Color> outline = role("outline");
      if (dark) {
        set("terminal_normal_black", role("surface_variant"));
        set("terminal_bright_black", outline);
        set("terminal_normal_white", role("on_surface_variant"));
        set("terminal_bright_white", role("on_surface"));
      } else {
        const std::optional<Color> surfaceVariant = role("surface_variant");
        set("terminal_normal_black", role("on_surface_variant"));
        set("terminal_bright_black", outline);
        // Halfway between bright black and bright white: no role sits there with every generator
        // (the custom schemes' outline_variant is as dark as outline).
        if (outline.has_value() && surfaceVariant.has_value()) {
          const Oklch low = toOklch(*outline);
          const Oklch high = toOklch(*surfaceVariant);
          set("terminal_normal_white",
              gamutMapToSrgb(
                  {(low.lightness + high.lightness) / 2.0, (low.chroma + high.chroma) / 2.0, high.hueRadians}
              ));
        }
        set("terminal_bright_white", surfaceVariant);
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
      if (!tokens->empty()) {
        applyPinnedHues(*tokens, base);
      }
    }
  }

} // namespace noctalia::theme
