#include "theme/scheme.h"

namespace noctalia::theme {

  std::optional<Scheme> schemeFromString(std::string_view s) {
    for (const auto& [name, scheme] : kSchemeEntries) {
      if (name == s)
        return scheme;
    }
    return std::nullopt;
  }

  std::string_view schemeToString(Scheme s) {
    for (const auto& [name, scheme] : kSchemeEntries) {
      if (scheme == s)
        return name;
    }
    return kSchemeEntries.front().first;
  }

  std::optional<TerminalScheme> terminalSchemeFromString(std::string_view s) {
    for (const auto& [name, scheme] : kTerminalSchemeEntries) {
      if (name == s)
        return scheme;
    }
    return std::nullopt;
  }

  std::string_view terminalSchemeToString(TerminalScheme s) {
    for (const auto& [name, scheme] : kTerminalSchemeEntries) {
      if (scheme == s)
        return name;
    }
    return kTerminalSchemeEntries.front().first;
  }

} // namespace noctalia::theme
