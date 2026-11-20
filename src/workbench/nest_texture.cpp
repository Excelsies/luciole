#include "workbench/nest_texture.h"

#include "workbench/treemap.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace luciole::workbench {
  namespace {
    constexpr double kTextureInset = 3;
    constexpr double kLeastTile = 16; // a tile this wide or tall, or less, has no texture

    bool texturedTile(const NestPiece& piece) { return piece.kind == NestPieceKind::Tile && piece.folder; }

    // Patches of one tile, the small ones left out.
    std::vector<NestPatch> tilePatches(const NestPiece& tile, const NestTextureSource& source) {
      const auto& r = tile.rect;
      // squarify gives nothing for a rectangle that is not finite.
      if (r.w <= kLeastTile || r.h <= kLeastTile || source.weights.size() != source.colors.size()) {
        return {};
      }
      const auto rects = squarify(source.weights, inset(r, kTextureInset));
      std::vector<NestPatch> patches;
      for (std::size_t i = 0; i < rects.size(); ++i) {
        if (rects[i].w >= kNestPatchLeast && rects[i].h >= kNestPatchLeast) {
          patches.push_back({.rect = rects[i], .color = source.colors[i]});
        }
      }
      return patches;
    }
  } // namespace

  NestTextureSource
  nestTextureSource(const Tree& tree, const LayoutSettings& settings, NodeId folder, const ColorPalette& palette) {
    if (!hasNode(tree, folder) || !tree.nodes[folder].folder) {
      return {};
    }
    const auto list = entries(tree, folder, settings);
    const std::size_t shown = std::min(list.ids.size(), kNestTextureEntries);
    const auto range = colorRange(tree, folder, settings.colorBy, settings.showHidden);
    NestTextureSource source;
    source.weights.assign(list.weights.begin(), list.weights.begin() + static_cast<std::ptrdiff_t>(shown));
    source.colors.reserve(shown);
    for (std::size_t i = 0; i < shown; ++i) {
      source.colors.push_back(nodeColor(tree, list.ids[i], settings.colorBy, range, palette));
    }
    return source;
  }

  std::vector<NodeId> missingNestSources(const NestScene& scene, const NestTextureSources& sources) {
    std::vector<NodeId> missing;
    for (const auto& piece : scene.pieces) {
      if (texturedTile(piece) && !sources.contains(piece.node)) {
        missing.push_back(piece.node);
      }
    }
    return missing;
  }

  std::vector<std::vector<NestPatch>> nestTexturePatches(const NestScene& scene, const NestTextureSources& sources) {
    std::vector<std::vector<NestPatch>> texture(scene.pieces.size());
    std::size_t placed = 0;
    for (std::size_t i = 0; i < scene.pieces.size(); ++i) {
      const auto& piece = scene.pieces[i];
      const auto source = texturedTile(piece) ? sources.find(piece.node) : sources.end();
      if (source == sources.end()) {
        continue;
      }
      auto patches = tilePatches(piece, source->second);
      if (placed + patches.size() > kNestTexturePatches) {
        break;
      }
      placed += patches.size();
      texture[i] = std::move(patches);
    }
    return texture;
  }
} // namespace luciole::workbench
