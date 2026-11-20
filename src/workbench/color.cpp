#include "workbench/color.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace luciole::workbench {
  namespace {
    struct CategoryExtensions {
      FileCategory category;
      std::string_view extensions; // space separated, lowercase
    };

    constexpr std::array<CategoryExtensions, 8> kCategoryExtensions{{
        {.category = FileCategory::Image,
         .extensions = "png jpg jpeg gif webp svg bmp ico tif tiff avif heic raw xcf psd"},
        {.category = FileCategory::Video, .extensions = "mp4 mkv webm avi mov wmv flv m4v mpg mpeg ogv"},
        {.category = FileCategory::Audio, .extensions = "mp3 flac wav ogg opus m4a aac wma mid midi"},
        {.category = FileCategory::Document,
         .extensions = "txt md rst pdf doc docx odt rtf tex epub djvu html htm xml ppt pptx odp xls xlsx ods"},
        {.category = FileCategory::Code,
         .extensions = "c h cc cpp cxx hpp hh py js mjs ts jsx tsx rs go java kt cs rb php pl lua swift css scss cmake "
                       "qml glsl vert frag toml ini cfg conf"},
        {.category = FileCategory::Archive, .extensions = "zip tar gz tgz bz2 xz zst 7z rar iso deb rpm pkg cab lz4"},
        {.category = FileCategory::Data,
         .extensions = "json csv tsv db sqlite sqlite3 yaml yml log dat bin sql parquet cache pak npy"},
        {.category = FileCategory::Executable, .extensions = "sh bash zsh fish exe bat cmd appimage run so a o dll ko"},
    }};

    // Whether `word` is one of the space separated words in `list`.
    bool listed(std::string_view list, std::string_view word) {
      // Each step moves past one word, so the loop runs at most list.size() times.
      while (!list.empty()) {
        const auto space = list.find(' ');
        if (list.substr(0, space) == word) {
          return true;
        }
        list = space == std::string_view::npos ? std::string_view{} : list.substr(space + 1);
      }
      return false;
    }

    std::uint8_t toByte(double value) {
      if (!(value > 0.0)) {
        return 0;
      }
      if (value >= 255.0) {
        return 255;
      }
      const double whole = std::floor(value);
      return static_cast<std::uint8_t>(value - whole >= 0.5 ? whole + 1.0 : whole);
    }

    double gammaToLinear(double channel) {
      const double v = channel / 255.0;
      return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    }

    double linearToGamma(double v) {
      return 255.0 * (v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055);
    }

    struct Linear {
      double r;
      double g;
      double b;
    };

    Linear oklchToLinear(double l, double c, double h) {
      const double a = c * std::cos(h);
      const double b = c * std::sin(h);
      const double lms1 = std::pow(l + 0.3963377774 * a + 0.2158037573 * b, 3.0);
      const double lms2 = std::pow(l - 0.1055613458 * a - 0.0638541728 * b, 3.0);
      const double lms3 = std::pow(l - 0.0894841775 * a - 1.291485548 * b, 3.0);
      return {
          .r = 4.0767416621 * lms1 - 3.3077115913 * lms2 + 0.2309699292 * lms3,
          .g = -1.2684380046 * lms1 + 2.6097574011 * lms2 - 0.3413193965 * lms3,
          .b = -0.0041960863 * lms1 - 0.7034186147 * lms2 + 1.7076147 * lms3,
      };
    }

    bool outsideGamut(const Linear& color) {
      const auto outside = [](double v) { return v < -0.0005 || v > 1.0005; };
      return outside(color.r) || outside(color.g) || outside(color.b);
    }

    FileCategory categoryOf(const Node& node) {
      return node.category < kFileCategoryCount ? static_cast<FileCategory>(node.category) : FileCategory::Other;
    }

    double colorValue(const Tree& tree, NodeId id, WorkbenchColor colorBy) {
      switch (colorBy) {
      case WorkbenchColor::Bytes:
        return measureValue(tree, id, WorkbenchMeasure::Bytes);
      case WorkbenchColor::Visits:
        return measureValue(tree, id, WorkbenchMeasure::Visits);
      case WorkbenchColor::Modified:
        return age(tree, id);
      case WorkbenchColor::Type:
        return 0.0;
      }
      return 0.0;
    }
  } // namespace

  std::string fileExtension(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= name.size()) {
      return {};
    }
    std::string extension(name.substr(dot + 1));
    for (auto& c : extension) {
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c - 'A' + 'a');
      }
    }
    return extension;
  }

  FileCategory fileCategory(std::string_view name, bool folder) {
    if (folder) {
      return FileCategory::Folder;
    }
    const auto extension = fileExtension(name);
    if (extension.empty()) {
      return FileCategory::Other;
    }
    for (const auto& entry : kCategoryExtensions) {
      if (listed(entry.extensions, extension)) {
        return entry.category;
      }
    }
    return FileCategory::Other;
  }

  double normalize(double value, double min, double max) {
    if (!(max > min)) {
      return 0.5;
    }
    const double position = (std::log1p(value) - std::log1p(min)) / (std::log1p(max) - std::log1p(min));
    return std::isnan(position) ? 0.5 : std::clamp(position, 0.0, 1.0);
  }

  Rgb mix(Rgb from, Rgb to, double amount) {
    const auto channel = [amount](std::uint8_t a, std::uint8_t b) {
      return toByte(static_cast<double>(a) + (static_cast<double>(b) - static_cast<double>(a)) * amount);
    };
    return {.r = channel(from.r, to.r), .g = channel(from.g, to.g), .b = channel(from.b, to.b)};
  }

  Rgb ramp(double position, const std::array<Rgb, 4>& stops) {
    const double x = (std::isnan(position) ? 0.0 : std::clamp(position, 0.0, 1.0)) * 3.0;
    const auto index = std::min<std::size_t>(2, static_cast<std::size_t>(std::floor(x)));
    return mix(stops[index], stops[index + 1], x - static_cast<double>(index));
  }

  double luminance(Rgb color) {
    const auto channel = [](std::uint8_t value) {
      const double v = static_cast<double>(value) / 255.0;
      return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.r) + 0.7152 * channel(color.g) + 0.0722 * channel(color.b);
  }

  double contrast(Rgb first, Rgb second) {
    const double a = luminance(first);
    const double b = luminance(second);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
  }

  Oklch toOklch(Rgb color) {
    const double r = gammaToLinear(color.r);
    const double g = gammaToLinear(color.g);
    const double b = gammaToLinear(color.b);
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    const double lightness = 0.2104542553 * l + 0.793617785 * m - 0.0040720468 * s;
    const double a = 1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s;
    const double bb = 0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s;
    return {.l = lightness, .c = std::hypot(a, bb), .h = std::atan2(bb, a)};
  }

  Rgb fromOklch(Oklch color) {
    double chroma = color.c;
    auto linear = oklchToLinear(color.l, chroma, color.h);
    for (int step = 0; step < 40 && outsideGamut(linear); ++step) {
      chroma *= 0.92;
      linear = oklchToLinear(color.l, chroma, color.h);
    }
    return {
        .r = toByte(linearToGamma(linear.r)), .g = toByte(linearToGamma(linear.g)), .b = toByte(linearToGamma(linear.b))
    };
  }

  Rgb fillFor(Rgb accent) {
    const auto oklch = toOklch(accent);
    const double k = std::clamp((oklch.l - 0.3) / 0.65, 0.0, 1.0);
    return fromOklch({.l = 0.3 + 0.16 * k, .c = std::min(oklch.c * 0.95, 0.15), .h = oklch.h});
  }

  Rgb inkFor(Rgb fill, const ColorPalette& palette) {
    return contrast(fill, palette.onSurface) >= contrast(fill, palette.surface) ? palette.onSurface : palette.surface;
  }

  ColorRange colorRange(const Tree& tree, NodeId folder, WorkbenchColor colorBy, bool showHidden) {
    double min = std::numeric_limits<double>::infinity();
    double max = -std::numeric_limits<double>::infinity();
    for (const auto id : children(tree, folder)) {
      if (hasNode(tree, id) && visibleEntry(tree.nodes[id], showHidden)) {
        const double value = colorValue(tree, id, colorBy);
        min = std::min(min, value);
        max = std::max(max, value);
      }
    }
    return min <= max ? ColorRange{.min = min, .max = max} : ColorRange{};
  }

  NodeColor
  nodeColor(const Tree& tree, NodeId id, WorkbenchColor colorBy, ColorRange range, const ColorPalette& palette) {
    if (!hasNode(tree, id)) {
      return {};
    }
    Rgb accent;
    if (colorBy == WorkbenchColor::Type) {
      accent = palette.categories[static_cast<std::size_t>(categoryOf(tree.nodes[id]))];
    } else {
      const double position = normalize(colorValue(tree, id, colorBy), range.min, range.max);
      accent = ramp(colorBy == WorkbenchColor::Modified ? 1.0 - position : position, palette.ramp);
    }
    const auto fill = fillFor(accent);
    return {.fill = fill, .ink = inkFor(fill, palette), .accent = accent};
  }

  NodeColor nodeColor(const Tree& tree, NodeId id, const LayoutSettings& settings, const ColorPalette& palette) {
    if (!hasNode(tree, id)) {
      return {};
    }
    const auto parent = tree.nodes[id].parent;
    const auto range =
        hasNode(tree, parent) ? colorRange(tree, parent, settings.colorBy, settings.showHidden) : ColorRange{};
    return nodeColor(tree, id, settings.colorBy, range, palette);
  }
} // namespace luciole::workbench
