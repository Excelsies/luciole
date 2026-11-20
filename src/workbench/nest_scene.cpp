#include "workbench/nest_scene.h"

#include "workbench/format.h"
#include "workbench/nest_rings.h"
#include "workbench/nest_tiles.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace luciole::workbench {
  namespace {
    // A tile is at least this wide: the label's 8 px padding on each side and room for a name cut to three code points
    // on each side of an ellipsis, the shortest name that tells entries apart.
    constexpr double kMinTileWidth = 64;
    constexpr std::array<double, 5> kNormalRings{40, 22, 12, 7, 4};
    constexpr std::array<double, 3> kCompactRings{16, 12, 12};
    constexpr std::size_t kSearchCap = 700;
    constexpr std::size_t kPeekTiles = 11;

    NestProfile normalProfile() {
      return {
          .compact = false,
          .ringThickness = kNormalRings,
          .lineThickness = 4,
          .gap = 3,
          .coreGap = 4,
          .pad = 8,
          .coreRadius = 6,
          .minTile = {.w = kMinTileWidth, .h = 28},
          .minPeekTile = {.w = 60, .h = 26},
          .minCore = {.w = 180, .h = 110},
          .peekMin = {.w = 340, .h = 230},
          .peekNeed = {.w = 180, .h = 120},
          .peekHead = 26,
      };
    }

    NestProfile compactProfile() {
      return {
          .compact = true,
          .ringThickness = kCompactRings,
          .lineThickness = 3,
          .gap = 2,
          .coreGap = 3,
          .pad = 4,
          .coreRadius = 4,
          .minTile = {.w = kMinTileWidth, .h = 24},
          .minPeekTile = {.w = 54, .h = 24},
          .minCore = {.w = 100, .h = 70},
          .peekMin = {.w = 220, .h = 150},
          .peekNeed = {.w = 160, .h = 120},
          .peekHead = 24,
      };
    }

    bool inRect(const TreemapRect& rect, double x, double y, double tolerance) {
      return x >= rect.x - tolerance
          && y >= rect.y - tolerance
          && x <= rect.x + rect.w + tolerance
          && y <= rect.y + rect.h + tolerance;
    }

    NestPiece makePiece(NestPieceKind kind, NodeId node) {
      NestPiece piece;
      piece.kind = kind;
      piece.node = node;
      return piece;
    }

    struct SceneInput {
      const Tree& tree;
      const LayoutSettings& settings;
      const ColorPalette& palette;
      const NestTextMeasure& nameWidth;
    };

    // Ancestors as given, up to the first id the tree does not hold.
    std::vector<NodeId> knownAncestors(const Tree& tree) {
      std::vector<NodeId> known;
      for (const auto id : tree.ancestors) {
        if (!hasNode(tree, id)) {
          break;
        }
        known.push_back(id);
      }
      return known;
    }

    // The bytes of the entries added up, and the most doubtful state among them.
    struct Sum {
      double bytes = 0;
      MeasureState state = MeasureState::Done;
    };

    Sum sumOf(const Tree& tree, std::span<const NodeId> ids) {
      Sum sum;
      for (const auto id : ids) {
        sum.bytes += measureValue(tree, id, WorkbenchMeasure::Bytes);
        sum.state = worseState(sum.state, measureState(tree.nodes[id]));
      }
      return sum;
    }

    // The room each of the first `shown` entries' names needs, or none without a measure.
    std::vector<double>
    nameNeeds(const Tree& tree, const Entries& list, const NestTextMeasure& measure, std::size_t shown) {
      std::vector<double> needs;
      if (!measure) {
        return needs;
      }
      const std::size_t count = std::min(shown, list.ids.size());
      needs.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        needs.push_back(narrowestNestTextWidth(tree.nodes[list.ids[i]].name, measure));
      }
      return needs;
    }

    // Tile and "+N more" pieces of a folder's entries laid out in `rect`.
    void addTilePieces(
        const SceneInput& in, std::vector<NestPiece>& pieces, const Entries& list, std::span<const double> weights,
        TreemapRect rect, NestSize min, std::size_t maxShown, NodeId folder, bool peek
    ) {
      const auto needs = nameNeeds(in.tree, list, in.nameWidth, maxShown);
      const auto layout = layoutNestTiles(weights, rect, min, maxShown, needs);
      const auto range = colorRange(in.tree, folder, in.settings.colorBy, in.settings.showHidden);
      for (std::size_t i = 0; i < layout.tiles.size(); ++i) {
        const auto id = list.ids[i];
        auto piece = makePiece(peek ? NestPieceKind::PeekTile : NestPieceKind::Tile, id);
        piece.rect = layout.tiles[i];
        piece.folder = in.tree.nodes[id].folder;
        piece.color = nodeColor(in.tree, id, in.settings.colorBy, range, in.palette);
        if (auto label = nestTileLabel(in.tree, in.settings, id, piece.rect)) {
          piece.label = std::move(*label);
        }
        pieces.push_back(std::move(piece));
      }
      if (layout.more) {
        auto piece = makePiece(peek ? NestPieceKind::PeekMore : NestPieceKind::More, folder);
        piece.nodes.assign(list.ids.begin() + static_cast<std::ptrdiff_t>(layout.tiles.size()), list.ids.end());
        piece.rect = *layout.more;
        const auto sum = sumOf(in.tree, piece.nodes);
        piece.label = nestMoreLabel(piece.nodes.size(), sum.bytes, sum.state, piece.rect);
        pieces.push_back(std::move(piece));
      }
    }

  } // namespace

  NestProfile nestProfile(double width, double height) {
    return width < 640 || height < 420 ? compactProfile() : normalProfile();
  }

  NestScene buildNestScene(
      const Tree& tree, const LayoutSettings& settings, double width, double height, const ColorPalette& palette,
      NestTextMeasure nameWidth
  ) {
    if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1) {
      NestScene scene;
      scene.profile = normalProfile();
      scene.empty = true;
      return scene;
    }
    const auto ancestors = knownAncestors(tree);
    const auto profile = nestProfile(width, height);
    auto plan = nestRingPlan(ancestors.size(), profile);
    auto frame = nestFrame(plan, width, height, profile);
    // At most one ring per pass, and the plan holds at most six.
    while (!plan.empty() && (frame.core.w < profile.minCore.w || frame.core.h < profile.minCore.h)) {
      plan.erase(plan.begin());
      frame = nestFrame(plan, width, height, profile);
    }
    NestScene scene;
    scene.width = width;
    scene.height = height;
    scene.folder = tree.root;
    scene.profile = profile;
    scene.core = frame.core.w > 0 && frame.core.h > 0 ? frame.core
                                                      : TreemapRect{
                                                            .x = profile.pad,
                                                            .y = profile.pad,
                                                            .w = std::max(1.0, width - 2 * profile.pad),
                                                            .h = std::max(1.0, height - 2 * profile.pad),
                                                        };
    scene.coreRadius = profile.coreRadius;
    const SceneInput in{.tree = tree, .settings = settings, .palette = palette, .nameWidth = nameWidth};
    const auto list = entries(tree, tree.root, settings);
    scene.empty = list.ids.empty();
    addTilePieces(in, scene.pieces, list, list.weights, scene.core, profile.minTile, kSearchCap, tree.root, false);
    addNestRings(tree, settings, palette, ancestors, plan, frame, scene);
    scene.nameWidth = std::move(nameWidth);
    return scene;
  }

  std::optional<std::size_t> nestHit(const NestScene& scene, double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
      return std::nullopt;
    }
    if (inRect(scene.core, x, y, 0)) {
      for (std::size_t i = 0; i < scene.pieces.size(); ++i) {
        const auto kind = scene.pieces[i].kind;
        if ((kind == NestPieceKind::Tile || kind == NestPieceKind::More) && inRect(scene.pieces[i].rect, x, y, 1)) {
          return i;
        }
      }
      return std::nullopt;
    }
    for (const auto& ring : scene.rings) {
      const auto s = locate(ring.band, x, y);
      if (!s || ring.pieceCount == 0) {
        continue;
      }
      // Last segment that starts at or before s.
      std::size_t low = ring.firstPiece;
      std::size_t high = ring.firstPiece + ring.pieceCount - 1;
      while (low < high) {
        const std::size_t mid = (low + high + 1) / 2;
        if (scene.pieces[mid].s0 <= *s) {
          low = mid;
        } else {
          high = mid - 1;
        }
      }
      return low;
    }
    return std::nullopt;
  }

  std::optional<std::size_t> nestFocus(const Tree& tree, const NestScene& scene, NodeId id) {
    auto current = id;
    // One step per node at most, so a cycle in the parent links ends.
    for (std::size_t step = 0; hasNode(tree, current) && step <= tree.nodes.size(); ++step) {
      if (current == scene.folder) {
        return std::nullopt;
      }
      const auto parent = tree.nodes[current].parent;
      if (parent == scene.folder) {
        for (std::size_t i = 0; i < scene.pieces.size(); ++i) {
          const auto& piece = scene.pieces[i];
          if ((piece.kind == NestPieceKind::Tile && piece.node == current)
              || (piece.kind == NestPieceKind::More && std::ranges::contains(piece.nodes, current))) {
            return i;
          }
        }
        return std::nullopt;
      }
      for (const auto& ring : scene.rings) {
        const auto first = ring.firstPiece;
        const auto last = ring.firstPiece + ring.pieceCount;
        for (std::size_t i = first; i < last; ++i) {
          if (scene.pieces[i].kind != NestPieceKind::MergedSegment && scene.pieces[i].node == current) {
            return i;
          }
        }
        for (std::size_t i = first; i < last; ++i) {
          const auto kind = scene.pieces[i].kind;
          if ((kind == NestPieceKind::MergedSegment || kind == NestPieceKind::LineSegment)
              && std::ranges::contains(scene.pieces[i].nodes, current)) {
            return i;
          }
        }
      }
      current = parent;
    }
    return std::nullopt;
  }

  bool nestPeekAllowed(const Tree& tree, const NestScene& scene, std::size_t piece) {
    if (piece >= scene.pieces.size() || scene.pieces[piece].kind != NestPieceKind::Tile) {
      return false;
    }
    const auto id = scene.pieces[piece].node;
    return hasNode(tree, id) && tree.nodes[id].folder && tree.nodes[id].items > 0;
  }

  std::optional<NestPeek> buildNestPeek(
      const Tree& tree, const LayoutSettings& settings, const NestScene& scene, std::size_t piece,
      const ColorPalette& palette
  ) {
    if (piece >= scene.pieces.size() || scene.pieces[piece].kind != NestPieceKind::Tile) {
      return std::nullopt;
    }
    const auto& tile = scene.pieces[piece];
    const auto& profile = scene.profile;
    const auto& core = scene.core;
    const auto kids = entries(tree, tile.node, settings);
    if (kids.ids.empty() || core.w < profile.peekNeed.w || core.h < profile.peekNeed.h) {
      return std::nullopt;
    }
    const double w = std::min(core.w, std::max(tile.rect.w, profile.peekMin.w));
    const double h = std::min(core.h, std::max(tile.rect.h, profile.peekMin.h));
    const double x = std::min(std::max(tile.rect.x + tile.rect.w / 2 - w / 2, core.x), core.x + core.w - w);
    const double y = std::min(std::max(tile.rect.y + tile.rect.h / 2 - h / 2, core.y), core.y + core.h - h);
    NestPeek peek;
    peek.node = tile.node;
    peek.from = tile.rect;
    peek.card = {.x = x, .y = y, .w = w, .h = h};
    peek.head = {.x = x, .y = y, .w = w, .h = profile.peekHead};
    peek.name = tree.nodes[tile.node].name;
    peek.nameAt = {.x = x + 12, .y = y + profile.peekHead / 2 + 1};
    peek.nameWidth = w - 150;
    peek.facts = itemsText(static_cast<double>(kids.ids.size()));
    if (const auto measure = measureLabel(tree, tile.node, settings.sizeBy); measure != peek.facts) {
      peek.facts += std::string(kFactsDot) + measure;
    }
    peek.factsAt = {.x = x + w - 12, .y = peek.nameAt.y};
    peek.count = kids.ids.size();
    auto head = makePiece(NestPieceKind::PeekHead, tile.node);
    head.rect = peek.head;
    head.folder = true;
    head.color = tile.color;
    peek.pieces.push_back(std::move(head));
    std::vector<double> weights(kids.weights.size());
    std::ranges::transform(kids.weights, weights.begin(), [](double value) { return std::sqrt(value); });
    const TreemapRect body{.x = x + 6, .y = y + profile.peekHead, .w = w - 12, .h = h - profile.peekHead - 6};
    const SceneInput in{.tree = tree, .settings = settings, .palette = palette, .nameWidth = scene.nameWidth};
    addTilePieces(in, peek.pieces, kids, weights, body, profile.minPeekTile, kPeekTiles, tile.node, true);
    return peek;
  }

  std::optional<std::size_t> nestPeekHit(const NestPeek& peek, double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y) || !inRect(peek.card, x, y, 0)) {
      return std::nullopt;
    }
    for (std::size_t i = 0; i < peek.pieces.size(); ++i) {
      if (peek.pieces[i].kind != NestPieceKind::PeekHead && inRect(peek.pieces[i].rect, x, y, 1)) {
        return i;
      }
    }
    return 0;
  }
} // namespace luciole::workbench
