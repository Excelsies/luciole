#include "ui/controls/code_editor_lines.h"

#include "formats/text_lines.h"
#include "imaging/text_tile_renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>

namespace luciole::code_editor {
  std::size_t codePoints(std::string_view text) {
    return formats::tokenize::codePoints(text, std::numeric_limits<std::size_t>::max());
  }

  formats::StyledLine withText(const formats::StyledLine& line, std::size_t column, std::string_view text) {
    formats::StyledLine out;
    out.continues = line.continues;
    bool placed = text.empty();
    std::size_t at = 0;
    for (const auto& run : line.runs) {
      const auto width = codePoints(run.text);
      if (!placed && column < at + width) {
        const auto split = formats::tokenize::prefixBytes(run.text, column - at);
        if (split > 0) {
          out.runs.push_back(
              {.text = run.text.substr(0, split), .role = run.role, .italic = run.italic, .bold = run.bold}
          );
        }
        out.runs.push_back({.text = std::string(text)});
        out.runs.push_back({.text = run.text.substr(split), .role = run.role, .italic = run.italic, .bold = run.bold});
        placed = true;
      } else {
        out.runs.push_back(run);
      }
      at += width;
    }
    if (!placed) {
      out.runs.push_back({.text = std::string(text)});
    }
    const auto added = static_cast<std::uint32_t>(codePoints(text));
    out.swatches = line.swatches;
    for (auto& swatch : out.swatches) {
      if (swatch.column >= column) {
        swatch.column += added;
      }
    }
    return out;
  }

  StripWindow stripWindow(int lineWidth, int left, int width) {
    if (lineWidth <= 0) {
      return {};
    }
    if (lineWidth <= kMaxTextTileWidth) {
      return {.x0 = 0, .width = lineWidth};
    }
    const auto center = static_cast<std::int64_t>(left) + (std::max(width, 0) / 2);
    const auto x0 = std::clamp<std::int64_t>(center - (kMaxTextTileWidth / 2), 0, lineWidth - kMaxTextTileWidth);
    return {.x0 = static_cast<int>(x0), .width = kMaxTextTileWidth};
  }

  std::vector<std::uint8_t> imageOf(const TextStrip& strip) {
    const auto width = static_cast<std::size_t>(std::max(strip.width, 0));
    const auto height = static_cast<std::size_t>(std::max(strip.height, 0));
    if (width == 0 || height == 0 || strip.rgba.size() != width * height * 4) {
      return {};
    }
    constexpr auto border = static_cast<std::size_t>(kStripBorder);
    const std::size_t stride = (width + (2 * border)) * 4;
    std::vector<std::uint8_t> out(stride * (height + (2 * border)), 0);
    // One step per pixel of the strip.
    for (std::size_t y = 0; y < height; ++y) {
      const auto* from = strip.rgba.data() + (y * width * 4);
      auto* to = out.data() + ((y + border) * stride) + (border * 4);
      for (std::size_t x = 0; x < width; ++x, from += 4, to += 4) {
        const unsigned alpha = from[3];
        to[3] = from[3];
        for (int c = 0; c < 3; ++c) {
          to[c] = alpha == 0 ? 0 : static_cast<std::uint8_t>(std::min(255U, ((from[c] * 255U) + (alpha / 2)) / alpha));
        }
      }
    }
    return out;
  }

  LineStrips::LineStrips(Node& layer) : m_layer(layer) {}

  LineStrips::~LineStrips() { clear(); }

  std::size_t LineStrips::bytes() const {
    std::size_t total = 0;
    for (const auto& [line, strip] : m_lines) {
      const auto pixels = static_cast<std::size_t>(strip.window.width + (2 * kStripBorder))
          * static_cast<std::size_t>(strip.height + (2 * kStripBorder));
      total += strip.node != nullptr ? pixels * 4 : 0;
    }
    return total;
  }

  bool LineStrips::current(std::size_t line, int left, int width) const {
    const auto found = m_lines.find(line);
    if (found == m_lines.end() || found->second.stale) {
      return false;
    }
    const auto& strip = found->second;
    auto start = static_cast<std::int64_t>(left);
    width = std::max(width, 0);
    if (width > kMaxTextTileWidth) {
      // No texture covers the whole viewport: the widest one centered on it, as stripWindow() draws, is enough.
      start += (width / 2) - (kMaxTextTileWidth / 2);
      width = kMaxTextTileWidth;
    }
    const auto end = start + width;
    const auto from = std::clamp<std::int64_t>(start, 0, strip.lineWidth);
    const auto to = std::clamp<std::int64_t>(end, 0, strip.lineWidth);
    return from >= strip.window.x0 && to <= static_cast<std::int64_t>(strip.window.x0) + strip.window.width;
  }

  void LineStrips::put(TextureManager& textures, std::size_t line, const TextStrip& strip, int lineWidth) {
    if (const auto found = m_lines.find(line); found != m_lines.end()) {
      remove(found, true);
    }
    Resident resident;
    resident.window = {.x0 = strip.x0, .width = strip.width};
    resident.height = strip.height;
    resident.lineWidth = lineWidth;
    const auto image = imageOf(strip);
    if (!image.empty()) {
      m_textures = &textures;
      const int width = strip.width + (2 * kStripBorder);
      const int height = strip.height + (2 * kStripBorder);
      auto handle = textures.loadFromRgba(image.data(), width, height);
      if (handle.valid()) {
        auto node = std::make_unique<ImageNode>();
        node->setHitTestVisible(false);
        node->setFitMode(ImageFitMode::Stretch);
        node->setTextureId(handle.id);
        node->setTextureSize(width, height);
        resident.texture = handle;
        resident.node = static_cast<ImageNode*>(m_layer.addChild(std::move(node)));
      }
    }
    m_lines.emplace(line, resident);
  }

  void LineStrips::markStale(std::size_t first, std::size_t count) {
    for (auto it = m_lines.lower_bound(first); it != m_lines.end() && it->first - first < count; ++it) {
      it->second.stale = true;
    }
  }

  void LineStrips::edited(std::size_t first, std::size_t removed, std::size_t inserted) {
    const std::size_t kept = std::min(removed, inserted);
    markStale(first, kept);
    // Each step removes one line of the replaced ones.
    for (auto it = m_lines.lower_bound(first + kept); it != m_lines.end() && it->first < first + removed;) {
      const auto next = std::next(it);
      remove(it, true);
      it = next;
    }
    if (removed == inserted) {
      return;
    }
    std::map<std::size_t, Resident> moved;
    // Each step moves one line that followed the replaced ones.
    for (auto it = m_lines.lower_bound(first + removed); it != m_lines.end();) {
      auto node = m_lines.extract(it++);
      node.key() = node.key() - removed + inserted;
      moved.insert(std::move(node));
    }
    m_lines.merge(moved);
  }

  void LineStrips::keepOnly(std::size_t first, std::size_t last) {
    // Each step removes one line or moves past one.
    for (auto it = m_lines.begin(); it != m_lines.end();) {
      const auto next = std::next(it);
      if (it->first < first || it->first > last) {
        remove(it, true);
      }
      it = next;
    }
  }

  void LineStrips::clear() {
    while (!m_lines.empty()) {
      remove(m_lines.begin(), true);
    }
  }

  void LineStrips::forget() {
    while (!m_lines.empty()) {
      remove(m_lines.begin(), false);
    }
  }

  void LineStrips::remove(std::map<std::size_t, Resident>::iterator it, bool unload) {
    auto& strip = it->second;
    if (unload && m_textures != nullptr && strip.texture.valid()) {
      m_textures->unload(strip.texture);
    }
    if (strip.node != nullptr) {
      (void)m_layer.removeChild(strip.node);
    }
    m_lines.erase(it);
  }

  void LineStrips::place(double scale, int originX, int originY, int lineHeight) {
    const double s = std::isfinite(scale) && scale > 0 ? scale : 1.0;
    for (auto& [line, strip] : m_lines) {
      if (strip.node == nullptr) {
        continue;
      }
      const double x = static_cast<double>(originX) + strip.window.x0 - kStripBorder;
      const double y = static_cast<double>(originY) + (static_cast<double>(line) * lineHeight) - kStripBorder;
      const double width = strip.window.width + (2.0 * kStripBorder);
      const double height = strip.height + (2.0 * kStripBorder);
      strip.node->setPosition(static_cast<float>(x / s), static_cast<float>(y / s));
      strip.node->setFrameSize(static_cast<float>(width / s), static_cast<float>(height / s));
    }
  }
} // namespace luciole::code_editor
