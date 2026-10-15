#include "render/core/wallpaper_polarity.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

  bool fail(std::string_view message) {
    std::println(stderr, "wallpaper_polarity_test: FAIL: {}", message);
    return false;
  }

  bool expect(bool condition, std::string_view message) {
    if (!condition)
      return fail(message);
    return true;
  }

  std::vector<std::uint8_t> solidImage(int width, int height, std::array<std::uint8_t, 3> rgb) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width * height * 4));
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
      pixels[i] = rgb[0];
      pixels[i + 1] = rgb[1];
      pixels[i + 2] = rgb[2];
      pixels[i + 3] = 255;
    }
    return pixels;
  }

  bool checkAnalysis() {
    using namespace wallpaper_polarity;
    bool ok = true;
    const auto dark = solidImage(640, 480, {20, 20, 30});
    const Analysis darkAnalysis = analyze(dark.data(), 640, 480, 4);
    ok = expect(!darkAnalysis.light, "a dark image is dark") && ok;
    ok =
        expect(std::abs(darkAnalysis.darkest - darkAnalysis.lightest) < 1e-4F, "a solid image has one lightness") && ok;

    const auto light = solidImage(64, 64, {240, 236, 220});
    ok = expect(analyze(light.data(), 64, 64, 4).light, "a light image is light") && ok;

    // A vertical gradient: the range spans most of it, without its extremes.
    std::vector<std::uint8_t> gradient(static_cast<std::size_t>(256 * 256 * 3));
    for (int y = 0; y < 256; ++y) {
      for (int x = 0; x < 256; ++x) {
        const std::size_t i = static_cast<std::size_t>((y * 256 + x) * 3);
        gradient[i] = gradient[i + 1] = gradient[i + 2] = static_cast<std::uint8_t>(y);
      }
    }
    const Analysis g = analyze(gradient.data(), 256, 256, 3);
    ok = expect(g.darkest > 0.0F && g.darkest < 0.15F, std::format("gradient's darkest ({})", g.darkest)) && ok;
    ok = expect(g.lightest > 0.9F && g.lightest < 1.0F, std::format("gradient's lightest ({})", g.lightest)) && ok;
    return ok;
  }

  bool checkTransform() {
    using namespace wallpaper_polarity;
    bool ok = true;
    const Analysis dark{.light = false, .darkest = 0.1F, .lightest = 0.4F};
    ok = expect(!transformFor(dark, false).invert, "a dark image stays in dark mode") && ok;
    const Transform t = transformFor(dark, true);
    ok =
        expect(t.invert && t.sourceLow == 0.1F && t.sourceHigh == 0.4F, "a dark image is inverted in light mode") && ok;
    ok = expect(t.targetLow > 0.4F && t.targetHigh > 0.9F, "light mode's target range is light") && ok;
    const Analysis light{.light = true, .darkest = 0.6F, .lightest = 0.95F};
    ok = expect(transformFor(light, false).invert, "a light image is inverted in dark mode") && ok;
    ok = expect(!transformFor(light, true).invert, "a light image stays in light mode") && ok;
    return ok;
  }

  // Reference values from the Python script this is ported from (wallpaper-polarity in the author's dotfiles).
  bool checkMatchesReference() {
    using namespace wallpaper_polarity;
    bool ok = true;
    const std::array<std::array<std::uint8_t, 3>, 6> input{{
        {0x20, 0x20, 0x30},
        {0xe0, 0x40, 0x50},
        {0x30, 0x90, 0x40},
        {0x10, 0x30, 0xc0},
        {0xf0, 0xe8, 0xd0},
        {0x80, 0x80, 0x80},
    }};
    struct Case {
      bool lightMode;
      Analysis analysis;
      std::array<std::array<int, 3>, 6> expected;
    };
    const std::array<Case, 2> cases{{
        {true,
         {.light = false, .darkest = 0.15F, .lightest = 0.45F},
         {{{184, 185, 206}, {147, 0, 35}, {0, 94, 27}, {34, 78, 220}, {84, 77, 57}, {77, 77, 77}}}},
        {false,
         {.light = true, .darkest = 0.55F, .lightest = 0.95F},
         {{{162, 162, 183}, {230, 70, 85}, {81, 174, 93}, {121, 161, 253}, {11, 7, 0}, {141, 141, 141}}}},
    }};
    for (const Case& c : cases) {
      std::vector<std::uint8_t> pixels;
      for (const auto& p : input) {
        pixels.insert(pixels.end(), p.begin(), p.end());
      }
      apply(pixels.data(), input.size(), 3, transformFor(c.analysis, c.lightMode));
      for (std::size_t i = 0; i < input.size(); ++i) {
        for (std::size_t ch = 0; ch < 3; ++ch) {
          const int got = pixels[i * 3 + ch];
          const int want = c.expected[i][ch];
          ok = expect(
                   std::abs(got - want) <= 1,
                   std::format(
                       "{} mode, pixel {} channel {}: {} (script: {})", c.lightMode ? "light" : "dark", i, ch, got, want
                   )
               )
              && ok;
        }
      }
    }
    return ok;
  }

  bool checkTable() {
    using namespace wallpaper_polarity;
    const auto& table = maxChromaTable();
    bool ok = expect(table.size() == static_cast<std::size_t>(kTableLightness * kTableHues), "table size");
    bool bounded = true;
    for (const float chroma : table) {
      bounded = bounded && chroma >= 0.0F && chroma < kTableMaxChroma;
    }
    ok = expect(bounded, "every entry is between 0 and the table's maximum") && ok;
    const float midRed = table[static_cast<std::size_t>(128 * kTableHues + 30)];
    ok =
        expect(midRed > 0.1F && midRed < kTableMaxChroma, std::format("mid-lightness red's chroma ({})", midRed)) && ok;
    return ok;
  }

  bool checkCache() {
    using namespace wallpaper_polarity;
    const auto path = std::filesystem::temp_directory_path() / "wallpaper_polarity_test.png";
    {
      std::ofstream(path) << "x";
    }
    bool ok = expect(!cached(path.string()).has_value(), "nothing cached at first");
    const Analysis first{.light = true, .darkest = 0.5F, .lightest = 0.9F};
    ok = expect(remember(path.string(), first) == first, "the first analysis is stored") && ok;
    const Analysis second{.light = false, .darkest = 0.1F, .lightest = 0.3F};
    ok = expect(remember(path.string(), second) == first, "the first analysis wins") && ok;
    ok = expect(cached(path.string()) == first, "cached returns it") && ok;
    std::filesystem::remove(path);
    return ok;
  }

} // namespace

int main() {
  bool ok = true;
  ok = checkAnalysis() && ok;
  ok = checkTransform() && ok;
  ok = checkMatchesReference() && ok;
  ok = checkTable() && ok;
  ok = checkCache() && ok;
  return ok ? 0 : 1;
}
