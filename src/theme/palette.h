#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace noctalia::theme {

  // A fully-generated palette, keyed by the token names from tokens.h. Values
  // are packed ARGB (0xffRRGGBB). Serialization to "#rrggbb" lives in
  // json_output.cpp.
  struct GeneratedPalette {
    std::unordered_map<std::string, uint32_t> dark;
    std::unordered_map<std::string, uint32_t> light;
    // A wallpaper palette's accent (materialSeed), for what follows the wallpaper's hue such as
    // the terminal schemes: not a token, since a scheme's source_color may be something else (Muted
    // picks quiet colors, the background on a dark wallpaper). Empty for other palettes.
    std::optional<uint32_t> accent;

    bool operator==(const GeneratedPalette&) const = default;
  };

} // namespace noctalia::theme
