#include "thumbnails/font_preview.h"

#include "fs/gio_ptr.h"
#include "fs/posix_fd.h"
#include "thumbnails/font_facts.h"
#include "thumbnails/font_sample.h"
#include "ui/palette.h"

#include <algorithm>
#include <array>
#include <cairo.h>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <format>
#include <map>
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  // The sheet is laid out in units of 1/320 of its width, so it keeps its proportions at every size and scale.
  constexpr double kSheetUnits = 320.0;
  constexpr std::array kSampleSizes{12.0, 18.0, 24.0, 36.0, 48.0};
  constexpr std::size_t kSheetFaces = 4;
  constexpr std::size_t kGridColumns = 8;
  using luciole::kMaxFontFileBytes;

  using ConfigPtr = std::unique_ptr<FcConfig, decltype(&FcConfigDestroy)>;
  using DescriptionPtr = std::unique_ptr<PangoFontDescription, decltype(&pango_font_description_free)>;
  using SurfacePtr = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
  using CairoPtr = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;

  std::uint32_t packed(const Color& color) {
    std::uint32_t value = 0;
    for (const float channel : {color.r, color.g, color.b}) {
      value = (value << 8) | static_cast<std::uint32_t>(std::lround(std::clamp(channel, 0.0F, 1.0F) * 255.0F));
    }
    return value;
  }
  std::optional<std::uint32_t> hexColor(std::string_view text) {
    std::uint32_t value = 0;
    if (text.size() != 6 || !std::ranges::all_of(text, [](char c) { return g_ascii_isxdigit(c) != 0; })) {
      return std::nullopt;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? std::optional(value) : std::nullopt;
  }
  void setColor(cairo_t* cairo, std::uint32_t color) {
    cairo_set_source_rgb(
        cairo, ((color >> 16) & 0xFFU) / 255.0, ((color >> 8) & 0xFFU) / 255.0, (color & 0xFFU) / 255.0
    );
  }
  std::string utf8(std::u32string_view text) {
    std::string result;
    std::array<char, 8> buffer{};
    for (const auto c : text) {
      result.append(buffer.data(), static_cast<std::size_t>(g_unichar_to_utf8(c, buffer.data())));
    }
    return result;
  }
  bool covered(FcPattern* face, std::string_view text) {
    FcCharSet* coverage = nullptr;
    if (text.empty()
        || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)
        || FcPatternGetCharSet(face, FC_CHARSET, 0, &coverage) != FcResultMatch) {
      return false;
    }
    for (const char* p = text.data(); p < text.data() + text.size(); p = g_utf8_next_char(p)) {
      if (!FcCharSetHasChar(coverage, g_utf8_get_char(p))) {
        return false;
      }
    }
    return true;
  }
  std::expected<std::vector<std::uint8_t>, std::string> readFont(const std::filesystem::path& file) {
    const luciole::Fd input(::open(file.c_str(), O_RDONLY | O_CLOEXEC));
    struct stat info{};
    if (!input.valid() || ::fstat(input.get(), &info) < 0 || !S_ISREG(info.st_mode)) {
      return std::unexpected("The font file cannot be opened.");
    }
    if (info.st_size <= 0 || static_cast<std::uint64_t>(info.st_size) > kMaxFontFileBytes) {
      return std::unexpected(info.st_size <= 0 ? "The font file is empty." : "The font file is too large.");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto count = ::pread(input.get(), bytes.data() + offset, bytes.size() - offset, static_cast<off_t>(offset));
      if (count <= 0) {
        return std::unexpected("The font file cannot be read.");
      }
      offset += static_cast<std::size_t>(count);
    }
    return bytes;
  }
  // Strike-only faces such as color emoji carry their bitmap size, which would pin every sample to it. Without it,
  // cairo scales their glyphs to the requested size, as the default configuration's bitmap scaling does.
  void releaseStrikeSizes(FcConfig* config) {
    const FcFontSet* fonts = FcConfigGetFonts(config, FcSetApplication);
    for (int i = 0; fonts && i < fonts->nfont; ++i) {
      (void)FcPatternDel(fonts->fonts[i], FC_PIXEL_SIZE);
    }
  }
  // The default instance of each face, in face order, from the private configuration's application fonts.
  std::vector<FcPattern*> sheetFaces(FcConfig* config) {
    std::map<int, FcPattern*> faces;
    const FcFontSet* fonts = FcConfigGetFonts(config, FcSetApplication);
    for (int i = 0; fonts && i < fonts->nfont; ++i) {
      int index = 0;
      FcBool variable = FcFalse;
      (void)FcPatternGetBool(fonts->fonts[i], FC_VARIABLE, 0, &variable);
      if (FcPatternGetInteger(fonts->fonts[i], FC_INDEX, 0, &index) == FcResultMatch && index >> 16 == 0 && !variable) {
        faces.emplace(index, fonts->fonts[i]);
      }
    }
    std::vector<FcPattern*> result;
    for (const auto& [_, face] : faces) {
      if (result.size() < kSheetFaces) {
        result.push_back(face);
      }
    }
    return result;
  }

  class SheetPainter {
  public:
    SheetPainter(cairo_t* cairo, PangoContext* context, int side)
        : m_cairo(cairo), m_context(context), m_side(side), m_unit(side / kSheetUnits), m_y(12 * m_unit) {}
    // Draws one line at `size` units unless the sheet is already full.
    void line(FcPattern* face, double size, std::string_view text) {
      if (m_y >= m_side) {
        return;
      }
      const auto layout = makeLayout(face, size, text);
      PangoRectangle extent{};
      pango_layout_get_pixel_extents(layout.get(), nullptr, &extent);
      // Right-to-left samples keep their start in view.
      const bool rightToLeft = pango_layout_get_direction(layout.get(), 0) == PANGO_DIRECTION_RTL;
      cairo_move_to(m_cairo, rightToLeft ? m_side - padding() - extent.x - extent.width : padding(), m_y);
      pango_cairo_show_layout(m_cairo, layout.get());
      m_y += extent.height + (4 * m_unit);
    }
    // Each character centered in its own cell, eight to a row.
    void grid(FcPattern* face, std::u32string_view text) {
      const double cell = (m_side - (2 * padding())) / static_cast<double>(kGridColumns);
      for (std::size_t i = 0; i < text.size() && m_y + ((i / kGridColumns) * cell) < m_side; ++i) {
        const auto layout = makeLayout(face, cell * 0.6 / m_unit, utf8(text.substr(i, 1)));
        PangoRectangle extent{};
        pango_layout_get_pixel_extents(layout.get(), nullptr, &extent);
        const double x = padding() + (static_cast<double>(i % kGridColumns) * cell) + ((cell - extent.width) / 2);
        const double y = m_y + (static_cast<double>(i / kGridColumns) * cell) + ((cell - extent.height) / 2);
        cairo_move_to(m_cairo, x, y);
        pango_cairo_show_layout(m_cairo, layout.get());
      }
      m_y += std::ceil(static_cast<double>(text.size()) / kGridColumns) * cell;
    }
    int height() const { return std::clamp(static_cast<int>(std::ceil(m_y + (8 * m_unit))), 1, m_side); }

  private:
    double padding() const { return 12 * m_unit; }
    luciole::GioPtr<PangoLayout> makeLayout(FcPattern* face, double size, std::string_view text) const {
      luciole::GioPtr<PangoLayout> layout(pango_layout_new(m_context));
      const DescriptionPtr description(
          pango_fc_font_description_from_pattern(face, FALSE), &pango_font_description_free
      );
      pango_font_description_set_absolute_size(description.get(), size * m_unit * PANGO_SCALE);
      pango_layout_set_font_description(layout.get(), description.get());
      pango_layout_set_single_paragraph_mode(layout.get(), TRUE);
      pango_layout_set_text(layout.get(), text.data(), static_cast<int>(text.size()));
      return layout;
    }

    cairo_t* m_cairo;
    PangoContext* m_context;
    int m_side;
    double m_unit;
    double m_y;
  };

  // Family and style, a sample at several sizes and digits, then the next faces of a collection.
  std::expected<int, std::string>
  paintSheet(cairo_t* cairo, PangoContext* context, const std::vector<FcPattern*>& faces, int side) {
    FcCharSet* coverage = nullptr;
    FcBool symbol = FcFalse;
    (void)FcPatternGetBool(faces.front(), FC_SYMBOL, 0, &symbol);
    if (FcPatternGetCharSet(faces.front(), FC_CHARSET, 0, &coverage) != FcResultMatch) {
      return std::unexpected("The font has no character map.");
    }
    const auto sample = luciole::fontSample(coverage, symbol != FcFalse);
    if (sample.text.empty()) {
      return std::unexpected("The font has no characters to show.");
    }
    const auto name = [](FcPattern* face) {
      FcChar8* family = nullptr;
      FcChar8* style = nullptr;
      (void)FcPatternGetString(face, FC_FAMILY, 0, &family);
      (void)FcPatternGetString(face, FC_STYLE, 0, &style);
      return std::format(
          "{} {}", family ? reinterpret_cast<const char*>(family) : "",
          style ? reinterpret_cast<const char*>(style) : ""
      );
    };
    SheetPainter painter(cairo, context, side);
    if (covered(faces.front(), name(faces.front()))) {
      painter.line(faces.front(), 14, name(faces.front()));
    }
    if (sample.grid) {
      painter.grid(faces.front(), sample.text);
    } else {
      const auto text = utf8(sample.text);
      for (const auto size : kSampleSizes) {
        painter.line(faces.front(), size, text);
      }
      if (covered(faces.front(), "0123456789")) {
        painter.line(faces.front(), 20, "0123456789");
      }
    }
    for (std::size_t i = 1; i < faces.size(); ++i) {
      if (covered(faces[i], name(faces[i]))) {
        painter.line(faces[i], 16, name(faces[i]));
      }
    }
    return painter.height();
  }

  LoadedImageFile straightRgba(cairo_surface_t* surface, int width, int height) {
    cairo_surface_flush(surface);
    const auto* data = cairo_image_surface_get_data(surface);
    const auto stride = static_cast<std::size_t>(cairo_image_surface_get_stride(surface));
    LoadedImageFile image{.rgba = {}, .width = width, .height = height, .sourceWidth = width, .sourceHeight = height};
    image.rgba.reserve(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t y = 0; y < static_cast<std::size_t>(height); ++y) {
      for (std::size_t x = 0; x < static_cast<std::size_t>(width); ++x) {
        std::uint32_t pixel = 0;
        std::memcpy(&pixel, data + (y * stride) + (x * 4), sizeof(pixel));
        const auto alpha = pixel >> 24;
        for (const int shift : {16, 8, 0}) {
          const auto channel = (pixel >> shift) & 0xFFU;
          image.rgba.push_back(
              static_cast<std::uint8_t>(alpha == 0 ? 0 : std::min(255U, ((channel * 255U) + (alpha / 2)) / alpha))
          );
        }
        image.rgba.push_back(static_cast<std::uint8_t>(alpha));
      }
    }
    return image;
  }

  bool writeResult(std::string_view path, std::span<const std::uint8_t> header, std::span<const std::uint8_t> body) {
    const luciole::Fd output(
        ::open(std::string(path).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)
    );
    for (const auto data : {header, body}) {
      for (std::size_t offset = 0; output.valid() && offset < data.size();) {
        const auto count = ::write(output.get(), data.data() + offset, data.size() - offset);
        if (count <= 0) {
          return false;
        }
        offset += static_cast<std::size_t>(count);
      }
    }
    return output.valid();
  }
} // namespace

std::string luciole::fontSheetVariant(std::string_view contentType) {
  if (!isFontContentType(contentType)) {
    return {};
  }
  return std::format(
      "{:06x}:{:06x}", packed(colorForRoleSnapshot(ColorRole::OnSurface)),
      packed(colorForRoleSnapshot(ColorRole::Surface))
  );
}

std::optional<luciole::FontSheetColors> luciole::parseFontSheetVariant(std::string_view variant) {
  if (variant.size() != 13 || variant[6] != ':') {
    return std::nullopt;
  }
  const auto text = hexColor(variant.substr(0, 6)), surface = hexColor(variant.substr(7));
  if (!text || !surface) {
    return std::nullopt;
  }
  return FontSheetColors{.text = *text, .surface = *surface};
}

std::expected<LoadedImageFile, std::string>
luciole::renderFontSheet(const std::filesystem::path& file, int side, FontSheetColors colors) {
  if (side < 1 || side > kMaxFontSheetSide) {
    return std::unexpected("invalid preview size");
  }
  const auto bytes = readFont(file);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  if (const auto container = checkFontFile(*bytes); !container) {
    return std::unexpected(container.error());
  }
  // Only this file's faces are visible to the sample, and nothing reaches the process-wide configuration.
  const ConfigPtr config(FcConfigCreate(), &FcConfigDestroy);
  if (!config || !FcConfigAppFontAddFile(config.get(), reinterpret_cast<const FcChar8*>(file.c_str()))) {
    return std::unexpected("The font file cannot be read.");
  }
  releaseStrikeSizes(config.get());
  const auto faces = sheetFaces(config.get());
  if (faces.empty()) {
    return std::unexpected("The font file cannot be read.");
  }
  const GioPtr<PangoFontMap> map(pango_cairo_font_map_new_for_font_type(CAIRO_FONT_TYPE_FT));
  if (!map) {
    return std::unexpected("Font rendering is unavailable.");
  }
  pango_fc_font_map_set_config(PANGO_FC_FONT_MAP(map.get()), config.get());
  const SurfacePtr surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, side, side), &cairo_surface_destroy);
  const CairoPtr cairo(cairo_create(surface.get()), &cairo_destroy);
  std::expected<int, std::string> height = std::unexpected("Font rendering is unavailable.");
  if (cairo_status(cairo.get()) == CAIRO_STATUS_SUCCESS) {
    const GioPtr<PangoContext> context(pango_font_map_create_context(map.get()));
    const std::unique_ptr<cairo_font_options_t, decltype(&cairo_font_options_destroy)> options(
        cairo_font_options_create(), &cairo_font_options_destroy
    );
    cairo_font_options_set_antialias(options.get(), CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options.get(), CAIRO_HINT_STYLE_NONE);
    cairo_font_options_set_hint_metrics(options.get(), CAIRO_HINT_METRICS_OFF);
    pango_cairo_context_set_font_options(context.get(), options.get());
    pango_cairo_update_context(cairo.get(), context.get());
    setColor(cairo.get(), colors.surface);
    cairo_paint(cairo.get());
    const double padding = 12 * side / kSheetUnits;
    cairo_rectangle(cairo.get(), padding, 0, side - (2 * padding), side);
    cairo_clip(cairo.get());
    setColor(cairo.get(), colors.text);
    height = paintSheet(cairo.get(), context.get(), faces, side);
  }
  pango_fc_font_map_shutdown(PANGO_FC_FONT_MAP(map.get()));
  if (!height) {
    return std::unexpected(height.error());
  }
  if (cairo_status(cairo.get()) != CAIRO_STATUS_SUCCESS) {
    return std::unexpected("Font rendering failed.");
  }
  return straightRgba(surface.get(), side, *height);
}

std::vector<std::string> luciole::fontPreviewArguments(
    int side, std::uint64_t maxBytes, std::string_view variant, const std::filesystem::path& output
) {
  return {"/proc/self/exe",         "--font-preview-worker", std::to_string(side),
          std::to_string(maxBytes), std::string(variant),    output.native()};
}

int luciole::runFontPreviewWorker(std::span<const std::string_view> arguments) {
  if (arguments.size() != 4) {
    return 2;
  }
  const auto number = [](std::string_view input) -> std::optional<std::uint64_t> {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size() ? std::optional(value) : std::nullopt;
  };
  const auto side = number(arguments[0]), maximum = number(arguments[1]);
  const auto colors = parseFontSheetVariant(arguments[2]);
  struct stat info{};
  if (!side
      || *side < 1
      || *side > kMaxFontSheetSide
      || !maximum
      || *maximum > kMaxFontFileBytes
      || !colors
      || ::fstat(3, &info) < 0
      || !S_ISREG(info.st_mode)
      || info.st_size <= 0
      || static_cast<std::uint64_t>(info.st_size) > *maximum) {
    return 2;
  }
  try {
    // An empty current configuration keeps Pango from loading the default one, with the user's font settings.
    const ConfigPtr empty(FcConfigCreate(), &FcConfigDestroy);
    if (!empty || !FcConfigSetCurrent(empty.get())) {
      return 1;
    }
    const auto image = renderFontSheet("/proc/self/fd/3", static_cast<int>(*side), *colors);
    if (!image) {
      constexpr std::array<std::uint8_t, 4> header{'N', 'F', 'E', '1'};
      const auto& message = image.error();
      (void)writeResult(arguments[3], header, {reinterpret_cast<const std::uint8_t*>(message.data()), message.size()});
      return 1;
    }
    std::array<std::uint8_t, 12> header{'N', 'F', 'R', '1'};
    for (std::size_t i = 0; i < 4; ++i) {
      header[4 + i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(image->width) >> (24 - (i * 8)));
      header[8 + i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(image->height) >> (24 - (i * 8)));
    }
    return writeResult(arguments[3], header, image->rgba) ? 0 : 1;
  } catch (const std::exception&) {
    return 1;
  }
}
