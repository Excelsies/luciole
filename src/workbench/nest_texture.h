#pragma once

#include "workbench/color.h"
#include "workbench/measure.h"
#include "workbench/nest_scene.h"
#include "workbench/tree.h"

#include <cstddef>
#include <unordered_map>
#include <vector>

// The faint texture of the Nest's folder tiles: each tile's heaviest entries laid out inside it. Sorting a folder's
// entries is the costly part, so it is done once per folder and snapshot into a source that outlives the layout.
namespace luciole::workbench {
  inline constexpr std::size_t kNestTextureEntries = 18;   // entries a tile's texture shows at most
  inline constexpr std::size_t kNestTexturePatches = 1200; // patches in a scene at most, heaviest tiles first
  inline constexpr double kNestPatchLeast = 5;             // a patch narrower or shorter than this is left out

  // Weights and colors of a folder's heaviest visible entries, heaviest first.
  struct NestTextureSource {
    std::vector<double> weights;
    std::vector<NodeColor> colors;
  };

  // Sources by folder, all for one snapshot, settings and palette.
  using NestTextureSources = std::unordered_map<NodeId, NestTextureSource>;

  // The source of a folder; empty for a file or an unknown id.
  NestTextureSource
  nestTextureSource(const Tree& tree, const LayoutSettings& settings, NodeId folder, const ColorPalette& palette);

  // Folders of the scene's tiles that have no source in `sources`, in piece order.
  std::vector<NodeId> missingNestSources(const NestScene& scene, const NestTextureSources& sources);

  // Patches of each folder tile that has a source, by piece index, laid out 3 px inside the tile; none for tiles of
  // 16 px or less either way. Patches under kNestPatchLeast either way are left out, and once kNestTexturePatches are
  // placed the remaining tiles get none.
  std::vector<std::vector<NestPatch>> nestTexturePatches(const NestScene& scene, const NestTextureSources& sources);
} // namespace luciole::workbench
