#include "render/core/wallpaper_polarity.h"

#include "core/log.h"
#include "render/core/image_encoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace wallpaper_polarity {

  namespace {

    constexpr Logger kLog("wallpaper-polarity");

    // Lightness range an inverted image is mapped onto, per the mode it is inverted for.
    constexpr TargetRange kLightTarget{.low = 0.42F, .high = 0.98F};
    constexpr TargetRange kDarkTarget{.low = 0.10F, .high = 0.72F};
    // Mean lightness above which an image counts as light.
    constexpr float kLightThreshold = 0.5F;
    constexpr int kSampleSide = 256;
    constexpr int kPreviewWidth = 512;
    // Part of the variant file names: bump when the pass's output changes, so files from an
    // earlier version aren't used.
    constexpr std::string_view kVariantVersion = "1";

    double decodeChannel(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }

    const std::array<double, 256>& decodeTable() {
      static const std::array<double, 256> table = [] {
        std::array<double, 256> t{};
        for (std::size_t i = 0; i < t.size(); ++i) {
          t[i] = decodeChannel(static_cast<double>(i) / 255.0);
        }
        return t;
      }();
      return table;
    }

    // OKLab lightness of an 8-bit sRGB pixel.
    float pixelLightness(const std::uint8_t* p) {
      const auto& decode = decodeTable();
      const double r = decode[p[0]];
      const double g = decode[p[1]];
      const double b = decode[p[2]];
      const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
      const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
      const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
      return static_cast<float>(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s);
    }

    float percentile(std::vector<float>& values, double fraction) {
      const auto index = static_cast<std::size_t>(std::lround(fraction * static_cast<double>(values.size() - 1)));
      std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
      return values[index];
    }

    std::filesystem::file_time_type modifiedTime(const std::string& path) {
      std::error_code ec;
      const auto time = std::filesystem::last_write_time(path, ec);
      return ec ? std::filesystem::file_time_type{} : time;
    }

    // ── Analyses ─────────────────────────────────────────────────────────────

    struct CacheEntry {
      Analysis analysis;
      std::filesystem::file_time_type modified;
    };

    std::mutex g_analysisMutex;
    std::unordered_map<std::string, CacheEntry> g_analyses;

    // ── Variants ─────────────────────────────────────────────────────────────

    std::filesystem::path variantDirectory() {
      if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "noctalia" / "wallpaper-variants";
      }
      if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".cache" / "noctalia" / "wallpaper-variants";
      }
      return std::filesystem::path("/tmp") / "noctalia" / "wallpaper-variants";
    }

    // Named after the image's path and modification time, so a changed file gets a new variant.
    std::filesystem::path variantPath(const std::string& path) {
      std::uint64_t hash = 14695981039346656037ULL; // FNV-1a
      const auto mix = [&hash](std::string_view bytes) {
        for (const char c : bytes) {
          hash = (hash ^ static_cast<std::uint8_t>(c)) * 1099511628211ULL;
        }
      };
      mix(path);
      mix(std::to_string(modifiedTime(path).time_since_epoch().count()));
      mix(kVariantVersion);
      return variantDirectory() / std::format("{:016x}.png", hash);
    }

    struct WriteJob {
      std::filesystem::path destination;
      std::vector<std::uint8_t> rgba;
      int width = 0;
      int height = 0;
    };

    // PNG encoding takes about a second for a 4K image, so one background thread writes the
    // variant files, in order.
    class VariantWriter {
    public:
      VariantWriter() : m_thread([this](const std::stop_token& stop) { run(stop); }) {}

      ~VariantWriter() {
        m_thread.request_stop();
        m_cv.notify_all();
      }

      VariantWriter(const VariantWriter&) = delete;
      VariantWriter& operator=(const VariantWriter&) = delete;

      void enqueue(WriteJob job) {
        {
          const std::lock_guard lock(m_mutex);
          m_jobs.push_back(std::move(job));
        }
        m_cv.notify_one();
      }

    private:
      void run(const std::stop_token& stop) {
        while (true) {
          WriteJob job;
          {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&] { return stop.stop_requested() || !m_jobs.empty(); });
            if (stop.stop_requested()) {
              return;
            }
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
          }
          write(job);
        }
      }

      static void write(const WriteJob& job) {
        std::string error;
        const auto png = encodePng(job.rgba.data(), job.width, job.height, &error);
        if (png.empty()) {
          kLog.warn("failed to encode {}: {}", job.destination.string(), error);
          return;
        }
        std::error_code ec;
        std::filesystem::create_directories(job.destination.parent_path(), ec);
        // Written next to its final place and renamed, so a reader never sees a partial file.
        const std::filesystem::path temporary = job.destination.string() + ".tmp";
        {
          std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
          out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
          if (!out) {
            kLog.warn("failed to write {}", temporary.string());
            return;
          }
        }
        std::filesystem::rename(temporary, job.destination, ec);
        if (ec) {
          kLog.warn("failed to write {}: {}", job.destination.string(), ec.message());
        }
      }

      std::mutex m_mutex;
      std::condition_variable m_cv;
      std::deque<WriteJob> m_jobs;
      std::jthread m_thread;
    };

    VariantWriter& variantWriter() {
      static VariantWriter writer;
      return writer;
    }

    std::mutex g_previewMutex;
    std::unordered_map<std::string, Preview> g_previews;

    std::map<std::uint64_t, Listener> g_listeners;
    std::uint64_t g_nextListener = 0;

    // A box-filtered copy at most kPreviewWidth wide.
    Preview makePreview(const std::vector<std::uint8_t>& rgba, int width, int height) {
      const int factor = std::max(1, (width + kPreviewWidth - 1) / kPreviewWidth);
      Preview preview{.width = std::max(1, width / factor), .height = std::max(1, height / factor)};
      preview.rgba.resize(static_cast<std::size_t>(preview.width) * static_cast<std::size_t>(preview.height) * 4U);
      const auto count = static_cast<unsigned>(factor * factor);
      for (int y = 0; y < preview.height; ++y) {
        for (int x = 0; x < preview.width; ++x) {
          std::array<unsigned, 4> sum{};
          for (int dy = 0; dy < factor; ++dy) {
            const std::size_t row = static_cast<std::size_t>(y * factor + dy) * static_cast<std::size_t>(width);
            for (int dx = 0; dx < factor; ++dx) {
              const std::size_t i = (row + static_cast<std::size_t>(x * factor + dx)) * 4U;
              for (std::size_t c = 0; c < 4; ++c) {
                sum[c] += rgba[i + c];
              }
            }
          }
          const std::size_t o =
              (static_cast<std::size_t>(y) * static_cast<std::size_t>(preview.width) + static_cast<std::size_t>(x))
              * 4U;
          for (std::size_t c = 0; c < 4; ++c) {
            preview.rgba[o + c] = static_cast<std::uint8_t>((sum[c] + count / 2) / count);
          }
        }
      }
      return preview;
    }

  } // namespace

  Analysis analyze(const std::uint8_t* pixels, int width, int height, int channels) {
    if (pixels == nullptr || width <= 0 || height <= 0 || channels < 3) {
      return {};
    }
    const int stepX = std::max(1, width / kSampleSide);
    const int stepY = std::max(1, height / kSampleSide);
    std::vector<float> lightness;
    lightness.reserve(static_cast<std::size_t>((width / stepX + 1) * (height / stepY + 1)));
    double sum = 0.0;
    for (int y = 0; y < height; y += stepY) {
      const std::uint8_t* row =
          pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
      for (int x = 0; x < width; x += stepX) {
        const float L = pixelLightness(row + static_cast<std::size_t>(x) * static_cast<std::size_t>(channels));
        lightness.push_back(L);
        sum += L;
      }
    }
    const double mean = sum / static_cast<double>(lightness.size());
    const float darkest = percentile(lightness, 0.02);
    const float lightest = percentile(lightness, 0.98);
    return {.light = mean > kLightThreshold, .darkest = darkest, .lightest = lightest};
  }

  TargetRange targetRangeFor(const Analysis& analysis) { return analysis.light ? kDarkTarget : kLightTarget; }

  std::optional<Analysis> cached(const std::string& path) {
    const auto modified = modifiedTime(path);
    const std::lock_guard lock(g_analysisMutex);
    const auto it = g_analyses.find(path);
    if (it == g_analyses.end() || it->second.modified != modified) {
      return std::nullopt;
    }
    return it->second.analysis;
  }

  Analysis remember(const std::string& path, const Analysis& analysis) {
    const auto modified = modifiedTime(path);
    const std::lock_guard lock(g_analysisMutex);
    auto [it, inserted] = g_analyses.try_emplace(path, CacheEntry{analysis, modified});
    if (!inserted && it->second.modified != modified) {
      it->second = CacheEntry{analysis, modified};
    }
    return it->second.analysis;
  }

  void storeVariant(const std::string& path, const std::vector<std::uint8_t>& rgba, int width, int height) {
    if (width <= 0
        || height <= 0
        || rgba.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U) {
      return;
    }
    {
      const std::lock_guard lock(g_previewMutex);
      g_previews[path] = makePreview(rgba, width, height);
    }
    if (const std::filesystem::path destination = variantPath(path); !std::filesystem::exists(destination)) {
      variantWriter().enqueue(WriteJob{.destination = destination, .rgba = rgba, .width = width, .height = height});
    }
    // Copied: a listener may add or remove listeners.
    const auto listeners = g_listeners;
    for (const auto& [id, listener] : listeners) {
      (void)id;
      listener(path);
    }
  }

  std::optional<Preview> variantPreview(const std::string& path) {
    const std::lock_guard lock(g_previewMutex);
    const auto it = g_previews.find(path);
    if (it == g_previews.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  std::optional<std::string> variantFile(const std::string& path) {
    const std::filesystem::path file = variantPath(path);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
      return std::nullopt;
    }
    return file.string();
  }

  std::uint64_t addVariantListener(Listener listener) {
    const std::uint64_t id = ++g_nextListener;
    g_listeners.emplace(id, std::move(listener));
    return id;
  }

  void removeVariantListener(std::uint64_t id) { g_listeners.erase(id); }

} // namespace wallpaper_polarity
