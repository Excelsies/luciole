#include "ui/controls/text_tile_view_tiles.h"

#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace luciole::text_tile_view {
  std::size_t tileBytes(const TextTileGeometry& geometry, std::size_t index) {
    if (index >= geometry.tileCount || geometry.width <= 0) {
      return 0;
    }
    const auto top = static_cast<std::int64_t>(index) * kTextTileHeight;
    const auto rows = std::clamp<std::int64_t>(geometry.height - top, 0, kTextTileHeight);
    return static_cast<std::size_t>(geometry.width) * static_cast<std::size_t>(rows) * 4;
  }

  TileWindow tileWindow(const TextTileGeometry& geometry, double top, double height, std::size_t budget) {
    TileWindow window;
    if (geometry.tileCount == 0
        || geometry.width <= 0
        || !std::isfinite(top)
        || !std::isfinite(height)
        || height <= 0) {
      return window;
    }
    const auto last = static_cast<double>(geometry.tileCount - 1);
    const auto tileAt = [&](double y) {
      return static_cast<std::size_t>(std::clamp(std::floor(y / kTextTileHeight), 0.0, last));
    };
    const auto first = tileAt(std::max(0.0, top));
    const auto end = tileAt(std::max(0.0, top + height - 1)) + 1;
    std::size_t used = 0;
    // At most end - first tiles, each within the layout.
    for (auto index = first; index < end; ++index) {
      used += tileBytes(geometry, index);
      window.tiles.push_back(index);
    }
    window.visible = window.tiles.size();
    const auto addExtra = [&](std::size_t index) {
      const auto bytes = tileBytes(geometry, index);
      if (used + bytes <= budget) {
        used += bytes;
        window.tiles.push_back(index);
      }
    };
    if (end < geometry.tileCount) {
      addExtra(end);
    }
    if (first > 0) {
      addExtra(first - 1);
    }
    return window;
  }

  TileTextures::TileTextures(Node& layer) : m_layer(layer) {}

  TileTextures::~TileTextures() { clear(); }

  std::size_t TileTextures::bytes() const {
    std::size_t total = 0;
    for (const auto& [index, tile] : m_tiles) {
      total += static_cast<std::size_t>(tile.width) * static_cast<std::size_t>(tile.height) * 4;
    }
    return total;
  }

  void TileTextures::upload(TextureManager& textures, std::size_t index, const TextTile& tile) {
    if (const auto found = m_tiles.find(index); found != m_tiles.end()) {
      remove(found, true);
    }
    const auto expected = static_cast<std::size_t>(tile.width) * static_cast<std::size_t>(tile.height) * 4;
    if (tile.width <= 0 || tile.height <= 0 || tile.rgba.size() != expected) {
      return;
    }
    m_textures = &textures;
    auto handle = textures.loadFromRgba(tile.rgba.data(), tile.width, tile.height);
    if (!handle.valid()) {
      return;
    }
    auto node = std::make_unique<ImageNode>();
    node->setHitTestVisible(false);
    node->setFitMode(ImageFitMode::Stretch);
    node->setTextureId(handle.id);
    node->setTextureSize(tile.width, tile.height);
    auto* image = static_cast<ImageNode*>(m_layer.addChild(std::move(node)));
    m_tiles[index] = {
        .texture = handle,
        .node = image,
        .top = tile.top,
        .width = tile.width,
        .height = tile.height,
        .swatches = tile.swatches,
    };
  }

  void TileTextures::keepOnly(const std::vector<std::size_t>& tiles) {
    // Each step erases one tile or moves past one.
    for (auto it = m_tiles.begin(); it != m_tiles.end();) {
      const auto next = std::next(it);
      if (!std::ranges::contains(tiles, it->first)) {
        remove(it, true);
      }
      it = next;
    }
  }

  void TileTextures::clear() {
    while (!m_tiles.empty()) {
      remove(m_tiles.begin(), true);
    }
  }

  void TileTextures::forget() {
    while (!m_tiles.empty()) {
      remove(m_tiles.begin(), false);
    }
  }

  void TileTextures::remove(std::map<std::size_t, Resident>::iterator it, bool unload) {
    auto& tile = it->second;
    if (unload && m_textures != nullptr && tile.texture.valid()) {
      m_textures->unload(tile.texture);
    }
    (void)m_layer.removeChild(tile.node);
    m_tiles.erase(it);
  }

  void TileTextures::place(double scale, float scrollX, float scrollY) {
    const double s = std::isfinite(scale) && scale > 0 ? scale : 1.0;
    const double left = -std::round(static_cast<double>(scrollX) * s);
    const double shift = std::round(static_cast<double>(scrollY) * s);
    for (auto& [index, tile] : m_tiles) {
      tile.node->setPosition(static_cast<float>(left / s), static_cast<float>((tile.top - shift) / s));
      tile.node->setFrameSize(static_cast<float>(tile.width / s), static_cast<float>(tile.height / s));
    }
  }

  std::optional<formats::Rgba> TileTextures::swatchAt(double x, double y) const {
    for (const auto& [index, tile] : m_tiles) {
      if (y < tile.top || y >= tile.top + tile.height) {
        continue;
      }
      for (const auto& swatch : tile.swatches) {
        if (x >= swatch.x && x < swatch.x + swatch.size && y >= swatch.y && y < swatch.y + swatch.size) {
          return swatch.color;
        }
      }
    }
    return std::nullopt;
  }
} // namespace luciole::text_tile_view
