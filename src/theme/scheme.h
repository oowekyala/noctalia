#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace noctalia::theme {

  // Color generation strategies. The first five are Material Design 3 schemes
  // (TonalPalette + tone tables, built on top of material_color_utilities).
  // The last five are custom HSL-space generators with very different
  // aesthetics — they are not Material You and will produce different output.
  enum class Scheme {
    TonalSpot,
    Content,
    FruitSalad,
    Rainbow,
    Monochrome,
    Vibrant,
    Faithful,
    Soft,
    Dysfunctional,
    Muted,
  };

  /// Generation strategies for the terminal colors only. Applied as a palette
  /// transform after the base scheme (see palette_transform.h), so it only replaces
  /// the terminal tokens and never changes how the wallpaper palette is cached.
  enum class TerminalScheme {
    /// Default keeps the terminal colors the base scheme produced.
    Default,
    /// PinnedHues replaces the normal and bright red, yellow, green, cyan, blue and
    /// magenta. Each color starts from a fixed hue, so that it stays in its own hue
    /// range, and is rotated towards the seed color's hue by at most a capped amount
    /// (Material's "harmonize"), less so when the seed is nearly gray. Its lightness is
    /// then searched for a contrast target on the final terminal background. Chroma and
    /// the size of the rotation caps depend on the base scheme (Muted and Soft are less
    /// saturated and rotate less, Vibrant is more saturated, Dysfunctional rotates more;
    /// Monochrome is nearly gray). Red and green keep a minimum chroma even with the muted
    /// schemes, so that eg git diffs stay legible, except with Monochrome, where they are as
    /// faint as the rest but still told apart. Black, white, foreground, cursor and selection
    /// are kept.
    PinnedHues
  };

  inline constexpr std::array<std::pair<std::string_view, Scheme>, 10> kSchemeEntries{{
      {"m3-tonal-spot", Scheme::TonalSpot},
      {"m3-content", Scheme::Content},
      {"m3-fruit-salad", Scheme::FruitSalad},
      {"m3-rainbow", Scheme::Rainbow},
      {"m3-monochrome", Scheme::Monochrome},
      {"vibrant", Scheme::Vibrant},
      {"faithful", Scheme::Faithful},
      {"soft", Scheme::Soft},
      {"dysfunctional", Scheme::Dysfunctional},
      {"muted", Scheme::Muted},
  }};

  inline constexpr std::array<std::pair<std::string_view, TerminalScheme>, 2> kTerminalSchemeEntries{{
      {"default", TerminalScheme::Default},
      {"pinned-hues", TerminalScheme::PinnedHues},
  }};

  inline constexpr std::array<std::string_view, kSchemeEntries.size()> kSchemeNames = [] {
    std::array<std::string_view, kSchemeEntries.size()> names{};
    for (std::size_t i = 0; i < kSchemeEntries.size(); ++i)
      names[i] = kSchemeEntries[i].first;
    return names;
  }();

  inline constexpr std::array<std::string_view, kTerminalSchemeEntries.size()> kTerminalSchemeNames = [] {
    std::array<std::string_view, kTerminalSchemeEntries.size()> names{};
    for (std::size_t i = 0; i < kTerminalSchemeEntries.size(); ++i)
      names[i] = kTerminalSchemeEntries[i].first;
    return names;
  }();

  // True for the Material Design 3 schemes.
  constexpr bool isMaterialScheme(Scheme s) {
    return s == Scheme::TonalSpot
        || s == Scheme::Content
        || s == Scheme::FruitSalad
        || s == Scheme::Rainbow
        || s == Scheme::Monochrome;
  }

  // Parse a scheme from its CLI string (e.g. "m3-tonal-spot", "vibrant").
  // Returns nullopt for unknown values.
  std::optional<Scheme> schemeFromString(std::string_view s);

  // String form used in CLI / JSON output.
  std::string_view schemeToString(Scheme s);

  // Same for the terminal scheme ("default", "pinned-hues").
  std::optional<TerminalScheme> terminalSchemeFromString(std::string_view s);
  std::string_view terminalSchemeToString(TerminalScheme s);

} // namespace noctalia::theme
