#include "render/core/wallpaper_polarity.h"
#include "render/programs/wallpaper_polarity_program.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
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
        const auto i = static_cast<std::size_t>((y * 256 + x) * 3);
        gradient[i] = gradient[i + 1] = gradient[i + 2] = static_cast<std::uint8_t>(y);
      }
    }
    const Analysis g = analyze(gradient.data(), 256, 256, 3);
    ok = expect(g.darkest > 0.0F && g.darkest < 0.15F, std::format("gradient's darkest ({})", g.darkest)) && ok;
    ok = expect(g.lightest > 0.9F && g.lightest < 1.0F, std::format("gradient's lightest ({})", g.lightest)) && ok;
    return ok;
  }

  bool checkPolarity() {
    using namespace wallpaper_polarity;
    bool ok = true;
    const Analysis dark{.light = false, .darkest = 0.1F, .lightest = 0.4F};
    const Analysis light{.light = true, .darkest = 0.6F, .lightest = 0.95F};
    ok =
        expect(!invertedIn(dark, false) && invertedIn(dark, true), "a dark image is inverted in light mode only") && ok;
    ok = expect(invertedIn(light, false) && !invertedIn(light, true), "a light image is inverted in dark mode only")
        && ok;
    ok =
        expect(targetRangeFor(dark).low > 0.4F && targetRangeFor(dark).high > 0.9F, "a dark image is made light") && ok;
    ok = expect(targetRangeFor(light).high < 0.8F, "a light image is made dark") && ok;
    return ok;
  }

  bool checkAnalysisCache() {
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

  bool checkVariantStore() {
    using namespace wallpaper_polarity;
    const auto cache = std::filesystem::temp_directory_path() / "wallpaper_polarity_test_cache";
    std::filesystem::remove_all(cache);
    ::setenv("XDG_CACHE_HOME", cache.c_str(), 1);
    const auto path = std::filesystem::temp_directory_path() / "wallpaper_polarity_test_variant.png";
    {
      std::ofstream(path) << "x";
    }

    int notified = 0;
    const auto listener = addVariantListener([&](const std::string& p) { notified += p == path.string() ? 1 : 0; });
    const auto pixels = solidImage(1200, 600, {200, 100, 50});
    storeVariant(path.string(), pixels, 1200, 600);
    removeVariantListener(listener);

    bool ok = expect(notified == 1, "listeners hear of the variant");
    const auto preview = variantPreview(path.string());
    ok = expect(preview.has_value() && preview->width <= 512 && preview->width > 0, "a small preview is kept") && ok;
    ok = expect(preview.has_value() && preview->rgba[0] == 200 && preview->rgba[1] == 100, "the preview is the image")
        && ok;
    // Written in the background.
    std::optional<std::string> file;
    for (int i = 0; i < 100 && !file.has_value(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      file = variantFile(path.string());
    }
    ok = expect(file.has_value() && file->starts_with(cache.string()), "the variant is written to the cache") && ok;
    std::filesystem::remove(path);
    std::filesystem::remove_all(cache);
    return ok;
  }

  // A surfaceless GLES2 context (Mesa), or nothing where there is no GPU to test with.
  struct HeadlessGl {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;

    bool create() {
      const auto getPlatformDisplay =
          reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
      if (getPlatformDisplay == nullptr) {
        return false;
      }
      display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
      if (display == EGL_NO_DISPLAY
          || eglInitialize(display, nullptr, nullptr) != EGL_TRUE
          || eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        return false;
      }
      // The default surface type is a window, which the surfaceless platform has none of.
      const std::array<EGLint, 5> configAttributes{
          EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE
      };
      EGLConfig config = nullptr;
      EGLint count = 0;
      if (eglChooseConfig(display, configAttributes.data(), &config, 1, &count) != EGL_TRUE || count == 0) {
        return false;
      }
      const std::array<EGLint, 3> contextAttributes{EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
      context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes.data());
      return context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) == EGL_TRUE;
    }

    ~HeadlessGl() {
      if (context != EGL_NO_CONTEXT) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
      }
      if (display != EGL_NO_DISPLAY) {
        eglTerminate(display);
      }
    }
  };

  GLuint texture(int width, int height, const std::uint8_t* pixels) {
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return id;
  }

  // The pass against a NumPy model of the same shader. The last four pixels are the compression noise of a "black"
  // background: in light mode they become the same untinted white as true black.
  bool checkPass(bool& skipped) {
    HeadlessGl gl;
    if (!gl.create()) {
      std::println(stderr, "wallpaper_polarity_test: no surfaceless GLES2 context; skipping the GPU pass");
      skipped = true;
      return true;
    }
    using wallpaper_polarity::Analysis;
    constexpr std::array<std::array<std::uint8_t, 3>, 10> input{{
        {0x20, 0x20, 0x30},
        {0xe0, 0x40, 0x50},
        {0x30, 0x90, 0x40},
        {0x10, 0x30, 0xc0},
        {0xf0, 0xe8, 0xd0},
        {0x80, 0x80, 0x80},
        {0, 0, 0},
        {1, 1, 1},
        {2, 0, 1},
        {4, 4, 4},
    }};
    struct Case {
      std::string_view name;
      Analysis analysis;
      std::array<std::array<int, 3>, 10> expected;
    };
    const std::array<Case, 2> cases{{
        {"dark image, light mode",
         {.light = false, .darkest = 0.15F, .lightest = 0.45F},
         {{{206, 207, 229},
           {147, 0, 35},
           {0, 94, 27},
           {39, 84, 227},
           {84, 77, 57},
           {77, 77, 77},
           {248, 248, 248},
           {248, 248, 248},
           {249, 248, 248},
           {248, 248, 248}}}},
        {"light image, dark mode",
         {.light = true, .darkest = 0.55F, .lightest = 0.95F},
         {{{162, 162, 183},
           {246, 85, 97},
           {94, 187, 105},
           {120, 161, 255},
           {17, 11, 0},
           {154, 154, 154},
           {164, 164, 164},
           {164, 164, 164},
           {165, 164, 165},
           {164, 164, 164}}}},
    }};

    std::vector<std::uint8_t> pixels;
    for (const auto& p : input) {
      pixels.insert(pixels.end(), {p[0], p[1], p[2], 255});
    }
    const int width = static_cast<int>(input.size());
    const GLuint source = texture(width, 1, pixels.data());
    bool ok = true;
    WallpaperPolarityProgram program;
    for (const Case& c : cases) {
      const GLuint target = texture(width, 1, nullptr);
      std::vector<std::uint8_t> out;
      ok =
          expect(program.render(source, target, width, 1, c.analysis, &out), std::format("{}: rendered", c.name)) && ok;
      for (std::size_t i = 0; i < input.size() && out.size() == pixels.size(); ++i) {
        for (std::size_t ch = 0; ch < 3; ++ch) {
          const int got = out[i * 4 + ch];
          const int want = c.expected[i][ch];
          ok = expect(
                   std::abs(got - want) <= 2,
                   std::format("{}, pixel {} channel {}: {} (model: {})", c.name, i, ch, got, want)
               )
              && ok;
        }
      }
      glDeleteTextures(1, &target);
    }
    program.destroy();
    glDeleteTextures(1, &source);
    return ok;
  }

} // namespace

int main() {
  bool ok = true;
  ok = checkAnalysis() && ok;
  ok = checkPolarity() && ok;
  ok = checkAnalysisCache() && ok;
  ok = checkVariantStore() && ok;
  bool skipped = false;
  ok = checkPass(skipped) && ok;
  return ok ? 0 : 1;
}
