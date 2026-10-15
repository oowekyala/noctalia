#include "theme/builtin_palettes.h"
#include "theme/color.h"
#include "theme/contrast.h"
#include "theme/palette_generator.h"
#include "theme/palette_transform.h"
#include "theme/scheme.h"

#include <array>
#include <cmath>
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

  bool checkTerminalSchemeStrings() {
    using noctalia::theme::TerminalScheme;
    bool ok = true;
    const auto pinned = noctalia::theme::terminalSchemeFromString("pinned-hues");
    ok = expect(pinned.has_value() && *pinned == TerminalScheme::PinnedHues, "parses pinned-hues") && ok;
    ok = expect(
             noctalia::theme::terminalSchemeToString(TerminalScheme::Default) == "default",
             "serializes the default terminal scheme"
         )
        && ok;
    ok = expect(!noctalia::theme::terminalSchemeFromString("pinned").has_value(), "rejects unknown terminal schemes")
        && ok;
    return ok;
  }

  double hueDegrees(uint32_t argb) {
    constexpr double kPi = 3.14159265358979323846;
    double degrees = noctalia::theme::toOklch(noctalia::theme::Color::fromArgb(argb)).hueRadians * 180.0 / kPi;
    return degrees < 0.0 ? degrees + 360.0 : degrees;
  }

  double hueDistance(double a, double b) {
    return std::abs(std::fmod(a - b + 540.0, 360.0) - 180.0);
  }

  bool checkPinnedHues() {
    using noctalia::theme::Scheme;
    using noctalia::theme::TerminalScheme;

    constexpr std::array schemes{
        Scheme::TonalSpot, Scheme::Content,  Scheme::FruitSalad, Scheme::Rainbow,       Scheme::Monochrome,
        Scheme::Vibrant,   Scheme::Faithful, Scheme::Soft,       Scheme::Dysfunctional, Scheme::Muted,
    };
    struct Slot {
      std::string_view name;
      double hue;
    };
    constexpr std::array<Slot, 6> slots{{
        {"red", 29.0}, {"yellow", 95.0}, {"green", 145.0}, {"cyan", 200.0}, {"blue", 260.0}, {"magenta", 330.0},
    }};
    constexpr std::array<std::string_view, 12> kept{
        "primary",           "surface",      "terminal_background", "terminal_foreground",
        "terminal_cursor",   "source_color", "terminal_normal_black", "terminal_normal_white",
        "terminal_bright_black", "terminal_bright_white", "terminal_selection_bg", "on_surface",
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
        const auto background = noctalia::theme::Color::fromArgb(after.at("terminal_background"));
        for (const std::string_view key : kept) {
          ok = expect(before.at(std::string(key)) == after.at(std::string(key)), label + ": keeps " + std::string(key))
              && ok;
        }
        for (const Slot& slot : slots) {
          for (const char* group : {"terminal_normal_", "terminal_bright_"}) {
            const std::string key = group + std::string(slot.name);
            const auto color = noctalia::theme::Color::fromArgb(after.at(key));
            ok = expect(noctalia::theme::contrastRatio(color, background) >= 4.5, label + ": " + key + " is readable")
                && ok;
          }
          const double hue = hueDistance(hueDegrees(after.at("terminal_normal_" + std::string(slot.name))), slot.hue);
          ok = expect(hue <= 55.0, label + ": " + std::string(slot.name) + " stays near its hue") && ok;
        }
        for (const std::string_view slot : {"red", "green"}) {
          const auto color = noctalia::theme::Color::fromArgb(after.at("terminal_normal_" + std::string(slot)));
          ok = expect(
                   noctalia::theme::toOklch(color).chroma >= 0.035, label + ": " + std::string(slot) + " keeps some chroma"
               )
              && ok;
        }
        // Red and green stay distinguishable (OKLab distance) even with the most muted schemes.
        const auto lab = [](uint32_t argb) {
          const auto lch = noctalia::theme::toOklch(noctalia::theme::Color::fromArgb(argb));
          return std::array<double, 3>{lch.lightness, lch.chroma * std::cos(lch.hueRadians), lch.chroma * std::sin(lch.hueRadians)};
        };
        const auto red = lab(after.at("terminal_normal_red"));
        const auto green = lab(after.at("terminal_normal_green"));
        const double distance = std::hypot(red[0] - green[0], std::hypot(red[1] - green[1], red[2] - green[2]));
        ok = expect(distance >= 0.06, label + ": red and green are distinguishable") && ok;
      };
      check(generated->dark, pinned.dark, "dark");
      check(generated->light, pinned.light, "light");

      auto withoutSource = *generated;
      withoutSource.dark.erase("source_color");
      const auto before = withoutSource;
      noctalia::theme::applyTerminalScheme(withoutSource, TerminalScheme::PinnedHues, scheme);
      ok = expect(withoutSource.dark == before.dark, name + ": a palette without a source color is left alone") && ok;
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
  ok = checkTerminalSchemeStrings() && ok;
  ok = checkPinnedHues() && ok;
  return ok ? 0 : 1;
}
