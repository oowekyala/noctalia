#pragma once

#include "theme/fixed_palette.h"
#include "theme/scheme.h"

#include <cstdint>

enum class TerminalScheme : std::uint8_t;

namespace noctalia::theme {

  // Re-anchors the dark surface ramp to true black: every surface-family token is
  // lowered by the tone of `surface`, so the background lands on tone 0 and the
  // elevation ladder keeps its relative steps, hue and chroma. Text, accents and
  // outlines are left alone, so contrast against the surfaces is preserved.
  //
  // Only the dark token map is touched — banding on OLED panels is a dark-background
  // problem, and a light palette has no true-black base to anchor to.
  void applyPureBlackDark(TokenMap& darkTokens);
  void applyPureBlackDark(GeneratedPalette& palette);

  void applyHighContrast(TokenMap& tokens, bool isDark);
  void applyHighContrast(GeneratedPalette& palette);

  // Replaces the sixteen normal and bright terminal colors according to `terminal` (see
  // TerminalScheme); `base` is the scheme the palette was generated with, which sets chroma and
  // rotation. Foreground, cursor and selection are kept. Meant to run after the other
  // transforms: the colors are searched against the final terminal_background. They follow the
  // palette's accent, or its source_color without one; a palette with neither, or without
  // terminal_background, and TerminalScheme::Default, are left as is.
  void applyTerminalScheme(GeneratedPalette& palette, TerminalScheme terminal, Scheme base);

} // namespace noctalia::theme
