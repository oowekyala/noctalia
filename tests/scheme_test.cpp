#include "config/config_types.h"
#include "theme/builtin_palettes.h"
#include "theme/color.h"
#include "theme/contrast.h"
#include "theme/palette_generator.h"
#include "theme/palette_transform.h"
#include "theme/scheme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

  bool fail(std::string_view message) {
    std::println(stderr, "scheme_test: FAIL: {}", message);
    return false;
  }

  bool expect(bool condition, std::string_view message) {
    if (!condition)
      return fail(message);
    return true;
  }

  std::vector<uint8_t> makeColorfulBuffer() {
    std::vector<uint8_t> rgb(112U * 112U * 3U);
    for (std::size_t y = 0; y < 112; ++y) {
      for (std::size_t x = 0; x < 112; ++x) {
        std::array<uint8_t, 3> color;
        if (x < 84) {
          color = {222, 59, 75};
        } else if (y < 56) {
          color = {50, 124, 222};
        } else {
          color = {236, 166, 51};
        }
        const std::size_t i = (y * 112U + x) * 3U;
        rgb[i + 0] = color[0];
        rgb[i + 1] = color[1];
        rgb[i + 2] = color[2];
      }
    }
    return rgb;
  }

  // A wallpaper of one vivid hue (HSL degrees), with a smaller patch of another.
  std::vector<uint8_t> makeHueBuffer(double hue) {
    const auto main = noctalia::theme::Color::fromHsl(hue, 0.65, 0.55);
    const auto other = noctalia::theme::Color::fromHsl(std::fmod(hue + 140.0, 360.0), 0.55, 0.45);
    std::vector<uint8_t> rgb(112U * 112U * 3U);
    for (std::size_t y = 0; y < 112; ++y) {
      for (std::size_t x = 0; x < 112; ++x) {
        const auto& color = x < 84 ? main : other;
        const std::size_t i = (y * 112U + x) * 3U;
        rgb[i + 0] = static_cast<uint8_t>(color.r);
        rgb[i + 1] = static_cast<uint8_t>(color.g);
        rgb[i + 2] = static_cast<uint8_t>(color.b);
      }
    }
    return rgb;
  }

  uint32_t token(const noctalia::theme::GeneratedPalette& palette, const std::string& name) {
    const auto it = palette.dark.find(name);
    return it == palette.dark.end() ? 0U : it->second;
  }

  double saturation(uint32_t argb) {
    const auto color = noctalia::theme::Color::fromArgb(argb);
    const auto [h, s, l] = color.toHsl();
    (void)h;
    (void)l;
    return s;
  }

  bool checkSchemeStrings() {
    using noctalia::theme::Scheme;
    bool ok = true;
    const auto parsed = noctalia::theme::schemeFromString("soft");
    ok = expect(parsed.has_value() && *parsed == Scheme::Soft, "parses canonical soft scheme") && ok;
    ok = expect(noctalia::theme::schemeToString(Scheme::Soft) == "soft", "serializes canonical soft scheme") && ok;
    ok = expect(!noctalia::theme::schemeFromString("softened").has_value(), "rejects non-canonical soft alias") && ok;
    return ok;
  }

  bool checkSoftGeneration() {
    using noctalia::theme::Scheme;

    const auto rgb = makeColorfulBuffer();
    const auto faithful = noctalia::theme::generateCustom(rgb, Scheme::Faithful);
    const auto soft = noctalia::theme::generateCustom(rgb, Scheme::Soft);
    const auto muted = noctalia::theme::generateCustom(rgb, Scheme::Muted);

    bool ok = true;
    const std::string required[] = {
        "source_color", "primary",   "on_primary", "surface",
        "on_surface",   "secondary", "tertiary",   "terminal_normal_red",
    };
    for (const auto& name : required) {
      ok = expect(soft.dark.contains(name), "soft dark palette has required token " + name) && ok;
      ok = expect(soft.light.contains(name), "soft light palette has required token " + name) && ok;
    }

    ok = expect(
             token(soft, "source_color") == token(faithful, "source_color"), "soft uses faithful source color selection"
         )
        && ok;

    const double mutedPrimary = saturation(token(muted, "primary"));
    const double softPrimary = saturation(token(soft, "primary"));
    const double faithfulPrimary = saturation(token(faithful, "primary"));
    ok = expect(
             mutedPrimary < softPrimary && softPrimary < faithfulPrimary,
             "soft primary saturation sits between muted and faithful"
         )
        && ok;

    const double mutedSurface = saturation(token(muted, "surface"));
    const double softSurface = saturation(token(soft, "surface"));
    const double faithfulSurface = saturation(token(faithful, "surface"));
    ok = expect(
             mutedSurface < softSurface && softSurface < faithfulSurface,
             "soft surface saturation sits between muted and faithful"
         )
        && ok;

    return ok;
  }

  bool checkContainerSaturationHeadroom() {
    bool ok = true;

    const auto* catppuccin = noctalia::theme::findBuiltinPalette("Catppuccin");
    if (!expect(catppuccin != nullptr, "finds Catppuccin builtin palette"))
      return false;

    const auto fixed = noctalia::theme::expandBuiltinPalette(*catppuccin);
    const auto fixedContainer = noctalia::theme::Color::fromArgb(token(fixed, "primary_container"));
    ok = expect(fixedContainer.toHex() == "#6d10da", "fixed palette container saturation scales by remaining headroom")
        && ok;

    const auto custom = noctalia::theme::generateCustom(makeColorfulBuffer(), noctalia::theme::Scheme::Faithful);
    const double customPrimarySaturation = saturation(token(custom, "primary"));
    const double customContainerSaturation = saturation(token(custom, "primary_container"));
    ok = expect(
             customContainerSaturation < customPrimarySaturation + 0.10,
             "custom palette container saturation scales by remaining headroom"
         )
        && ok;

    return ok;
  }

  bool checkPerceptualContrastAdjustment() {
    const auto background = noctalia::theme::Color::fromHex("#fbf8ff");
    const auto paleLavender = noctalia::theme::Color::fromHex("#bdc2ff");
    const auto adjusted = noctalia::theme::ensureContrast(paleLavender, background, 4.5);
    bool ok = true;
    ok = expect(
             noctalia::theme::contrastRatio(adjusted, background) >= 4.5,
             "OKLCH-adjusted foreground has readable contrast"
         )
        && ok;
    ok = expect(adjusted.toHex() == "#6b6ea6", "OKLCH adjustment preserves pale lavender character") && ok;

    const auto readable = noctalia::theme::Color::fromHex("#5158a1");
    ok = expect(
             noctalia::theme::ensureContrast(readable, background, 4.5).toArgb() == readable.toArgb(),
             "already-readable foreground remains unchanged"
         )
        && ok;

    const auto impossibleBackground = noctalia::theme::Color::fromHex("#777777");
    const auto impossibleForeground = noctalia::theme::Color::fromHex("#888888");
    const auto bestPossible = noctalia::theme::ensureContrast(impossibleForeground, impossibleBackground, 7.0, 1);
    ok = expect(bestPossible.toHex() == "#000000", "impossible target returns the best-contrast endpoint") && ok;
    return ok;
  }

  bool checkTerminalContrast() {
    using noctalia::theme::Scheme;

    constexpr std::array schemes{
        Scheme::TonalSpot, Scheme::Content,  Scheme::FruitSalad, Scheme::Rainbow,       Scheme::Monochrome,
        Scheme::Vibrant,   Scheme::Faithful, Scheme::Soft,       Scheme::Dysfunctional, Scheme::Muted,
    };
    constexpr std::array<std::string_view, 12> colorKeys{
        "terminal_normal_red",     "terminal_normal_green", "terminal_normal_yellow",  "terminal_normal_blue",
        "terminal_normal_magenta", "terminal_normal_cyan",  "terminal_bright_red",     "terminal_bright_green",
        "terminal_bright_yellow",  "terminal_bright_blue",  "terminal_bright_magenta", "terminal_bright_cyan",
    };
    constexpr std::array<std::string_view, 4> accentKeys{"primary", "secondary", "tertiary", "error"};

    const auto rgb = makeColorfulBuffer();
    bool ok = true;
    for (const Scheme scheme : schemes) {
      const auto generated = noctalia::theme::generate(rgb, scheme);
      if (!expect(generated.has_value(), std::string(noctalia::theme::schemeToString(scheme)) + " palette generated")) {
        ok = false;
        continue;
      }

      const auto checkMode = [&](const auto& tokens, std::string_view mode) {
        const auto backgroundIt = tokens.find("terminal_background");
        if (!expect(
                backgroundIt != tokens.end(),
                std::string(noctalia::theme::schemeToString(scheme))
                    + " "
                    + std::string(mode)
                    + " terminal background exists"
            )) {
          return false;
        }

        const auto background = noctalia::theme::Color::fromArgb(backgroundIt->second);
        bool modeOk = true;
        for (const std::string_view key : colorKeys) {
          const auto colorIt = tokens.find(std::string(key));
          if (!expect(
                  colorIt != tokens.end(),
                  std::string(noctalia::theme::schemeToString(scheme))
                      + " "
                      + std::string(mode)
                      + " "
                      + std::string(key)
                      + " exists"
              )) {
            modeOk = false;
            continue;
          }
          const auto color = noctalia::theme::Color::fromArgb(colorIt->second);
          modeOk = expect(
                       noctalia::theme::contrastRatio(color, background) >= 4.5,
                       std::string(noctalia::theme::schemeToString(scheme))
                           + " "
                           + std::string(mode)
                           + " "
                           + std::string(key)
                           + " has readable contrast"
                   )
              && modeOk;
        }

        for (const std::string_view key : accentKeys) {
          const auto backgroundColorIt = tokens.find(std::string(key));
          const auto foregroundColorIt = tokens.find("on_" + std::string(key));
          if (!expect(
                  backgroundColorIt != tokens.end() && foregroundColorIt != tokens.end(),
                  std::string(noctalia::theme::schemeToString(scheme))
                      + " "
                      + std::string(mode)
                      + " "
                      + std::string(key)
                      + " pair exists"
              )) {
            modeOk = false;
            continue;
          }
          const auto backgroundColor = noctalia::theme::Color::fromArgb(backgroundColorIt->second);
          const auto foregroundColor = noctalia::theme::Color::fromArgb(foregroundColorIt->second);
          modeOk = expect(
                       noctalia::theme::contrastRatio(foregroundColor, backgroundColor) >= 4.5,
                       std::string(noctalia::theme::schemeToString(scheme))
                           + " "
                           + std::string(mode)
                           + " "
                           + std::string(key)
                           + " foreground has readable contrast"
                   )
              && modeOk;
        }
        return modeOk;
      };

      ok = checkMode(generated->dark, "dark") && ok;
      ok = checkMode(generated->light, "light") && ok;
    }
    return ok;
  }

  // Pinned hues only replaces the sixteen terminal colors: the six chromatic ones readable on the
  // terminal background, with red and green told apart even by the most muted schemes, and black
  // and white ordered from dark to light, black readable in light mode.
  bool checkPinnedHues() {
    using noctalia::theme::Scheme;

    constexpr std::array schemes{
        Scheme::TonalSpot, Scheme::Content,  Scheme::FruitSalad, Scheme::Rainbow,       Scheme::Monochrome,
        Scheme::Vibrant,   Scheme::Faithful, Scheme::Soft,       Scheme::Dysfunctional, Scheme::Muted,
    };
    constexpr std::array<std::string_view, 6> slots{"red", "yellow", "green", "cyan", "blue", "magenta"};
    constexpr std::array<std::string_view, 4> neutrals{
        "terminal_normal_black", "terminal_bright_black", "terminal_normal_white", "terminal_bright_white"
    };
    const auto lightness = [](uint32_t argb) {
      return noctalia::theme::toOklch(noctalia::theme::Color::fromArgb(argb)).lightness;
    };

    bool ok = true;
    const auto rgb = makeColorfulBuffer();
    for (const Scheme scheme : schemes) {
      const std::string name{noctalia::theme::schemeToString(scheme)};
      const auto generated = noctalia::theme::generate(rgb, scheme);
      if (!expect(generated.has_value(), name + " palette generated")) {
        ok = false;
        continue;
      }
      auto unchanged = *generated;
      noctalia::theme::applyTerminalScheme(unchanged, TerminalScheme::Default, scheme);
      ok = expect(unchanged == *generated, name + ": the default terminal scheme changes nothing") && ok;

      auto pinned = *generated;
      noctalia::theme::applyTerminalScheme(pinned, TerminalScheme::PinnedHues, scheme);
      const auto check = [&](const auto& before, const auto& after, std::string_view mode) {
        const std::string label = name + " " + std::string(mode);
        for (const auto& [key, value] : before) {
          const bool replaced = std::ranges::any_of(
                                    slots,
                                    [&](std::string_view slot) {
                                      return key == "terminal_normal_" + std::string(slot)
                                          || key == "terminal_bright_" + std::string(slot);
                                    }
                                )
              || std::ranges::find(neutrals, key) != neutrals.end();
          ok = expect(replaced || after.at(key) == value, label + ": keeps " + key) && ok;
        }
        const auto background = noctalia::theme::Color::fromArgb(after.at("terminal_background"));
        for (const std::string_view slot : slots) {
          for (const char* group : {"terminal_normal_", "terminal_bright_"}) {
            const std::string key = group + std::string(slot);
            const auto color = noctalia::theme::Color::fromArgb(after.at(key));
            ok = expect(noctalia::theme::contrastRatio(color, background) >= 4.5, label + ": " + key + " is readable")
                && ok;
          }
        }
        // Red and green stay distinguishable (OKLab distance), for diffs and test output.
        const auto lab = [](uint32_t argb) {
          const auto lch = noctalia::theme::toOklch(noctalia::theme::Color::fromArgb(argb));
          return std::array<double, 3>{
              lch.lightness, lch.chroma * std::cos(lch.hueRadians), lch.chroma * std::sin(lch.hueRadians)
          };
        };
        const auto red = lab(after.at("terminal_normal_red"));
        const auto green = lab(after.at("terminal_normal_green"));
        const double distance = std::hypot(red[0] - green[0], std::hypot(red[1] - green[1], red[2] - green[2]));
        ok = expect(distance >= 0.06, label + ": red and green are distinguishable") && ok;

        for (std::size_t i = 0; i + 1 < neutrals.size(); ++i) {
          ok = expect(
                   lightness(after.at(std::string(neutrals[i]))) < lightness(after.at(std::string(neutrals[i + 1]))),
                   label + ": " + std::string(neutrals[i]) + " is darker than " + std::string(neutrals[i + 1])
               )
              && ok;
        }
        if (mode == "light") {
          const auto black = noctalia::theme::Color::fromArgb(after.at("terminal_normal_black"));
          ok = expect(noctalia::theme::contrastRatio(black, background) >= 4.5, label + ": black is readable") && ok;
        }
      };
      check(generated->dark, pinned.dark, "dark");
      check(generated->light, pinned.light, "light");
    }

    // Whatever the wallpaper's hue, neighbouring colors stay apart in hue, and every bright color
    // differs from its normal one.
    for (int hue = 0; hue < 360; hue += 15) {
      const auto buffer = makeHueBuffer(hue);
      for (const Scheme scheme : schemes) {
        auto palette = noctalia::theme::generate(buffer, scheme);
        if (!palette.has_value()) {
          continue;
        }
        noctalia::theme::applyTerminalScheme(*palette, TerminalScheme::PinnedHues, scheme);
        for (const auto* tokens : {&palette->dark, &palette->light}) {
          const std::string label = std::string(noctalia::theme::schemeToString(scheme))
              + " "
              + (tokens == &palette->dark ? "dark" : "light")
              + " on hue "
              + std::to_string(hue);
          std::array<double, slots.size()> hues{};
          for (std::size_t i = 0; i < slots.size(); ++i) {
            const auto normal = tokens->at("terminal_normal_" + std::string(slots[i]));
            const auto bright = tokens->at("terminal_bright_" + std::string(slots[i]));
            ok = expect(normal != bright, label + ": bright " + std::string(slots[i]) + " differs from normal") && ok;
            hues[i] = noctalia::theme::toOklch(noctalia::theme::Color::fromArgb(normal)).hueRadians
                * 180.0
                / std::numbers::pi;
          }
          for (std::size_t i = 0; i < slots.size(); ++i) {
            const double gap = std::fmod(hues[(i + 1) % slots.size()] - hues[i] + 720.0, 360.0);
            ok = expect(gap >= 25.0, label + ": " + std::string(slots[i]) + " and the next color are apart in hue")
                && ok;
          }
        }
      }
    }
    return ok;
  }

} // namespace

int main() {
  bool ok = true;
  ok = checkSchemeStrings() && ok;
  ok = checkSoftGeneration() && ok;
  ok = checkContainerSaturationHeadroom() && ok;
  ok = checkPerceptualContrastAdjustment() && ok;
  ok = checkTerminalContrast() && ok;
  ok = checkPinnedHues() && ok;
  return ok ? 0 : 1;
}
