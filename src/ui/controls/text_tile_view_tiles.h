#pragma once

#include "formats/color_codes.h"
#include "imaging/text_tile_renderer.h"
#include "imaging/text_tile_service.h"
#include "render/core/texture_handle.h"

#include <cstddef>
#include <map>
#include <optional>
#include <vector>

class ImageNode;
class Node;
class TextureManager;

namespace luciole::text_tile_view {
  // Bytes of the texture of one tile; zero past the layout's tiles.
  std::size_t tileBytes(const TextTileGeometry& geometry, std::size_t index);

  // The tiles a viewport wants, the ones it shows first.
  struct TileWindow {
    std::vector<std::size_t> tiles;
    std::size_t visible = 0; // tiles[0, visible) cross the viewport
  };

  // The tiles crossing device rows [top, top + height) of the layout, top down, then one below and one above while
  // the textures stay within `budget` bytes. The budget never cuts a tile that crosses the rows.
  TileWindow tileWindow(const TextTileGeometry& geometry, double top, double height, std::size_t budget);

  // The textures of one layout's tiles, each drawn by an image node of `layer`, which it owns. Destroying it, or
  // clear(), unloads them.
  class TileTextures {
  public:
    explicit TileTextures(Node& layer);
    ~TileTextures();
    TileTextures(const TileTextures&) = delete;
    TileTextures& operator=(const TileTextures&) = delete;

    bool holds(std::size_t index) const { return m_tiles.contains(index); }
    std::size_t count() const { return m_tiles.size(); }
    std::size_t bytes() const;

    // Uploads the opaque tile, replacing one of the same index.
    void upload(TextureManager& textures, std::size_t index, const TextTile& tile);
    // Unloads every tile not listed.
    void keepOnly(const std::vector<std::size_t>& tiles);
    void clear();
    // Drops the tiles without unloading them, after the GPU lost every texture.
    void forget();

    // Places the image nodes of a layout drawn at `scale`, scrolled by logical pixels, on whole device pixels.
    void place(double scale, float scrollX, float scrollY);

    // The color of the swatch at a point of the layout, in device pixels.
    std::optional<formats::Rgba> swatchAt(double x, double y) const;

  private:
    struct Resident {
      TextureHandle texture;
      ImageNode* node = nullptr;
      int top = 0;
      int width = 0;
      int height = 0;
      std::vector<TextSwatchBox> swatches;
    };

    void remove(std::map<std::size_t, Resident>::iterator it, bool unload);

    Node& m_layer;
    TextureManager* m_textures = nullptr;
    std::map<std::size_t, Resident> m_tiles;
  };
} // namespace luciole::text_tile_view
