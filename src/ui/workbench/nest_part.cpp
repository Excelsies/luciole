#include "ui/workbench/nest_part.h"

#include "core/input/key_symbols.h"
#include "core/timer_manager.h"
#include "ui/workbench/entry_tooltip.h"
#include "ui/workbench/nest_work.h"
#include "workbench/nest_card.h"
#include "workbench/nest_keys.h"
#include "workbench/nest_labels.h"
#include "workbench/nest_scene.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace luciole {
  namespace {
    using workbench::NestPieceKind;
    using workbench::NodeId;
    using workbench::SceneRole;
    using workbench::ShapeId;

    constexpr float kTransitionMs = 260;
    constexpr auto kDwell = std::chrono::milliseconds(workbench::kNestPeekDwellMs);

    PartReaction handled(std::optional<PartRequest> request = std::nullopt) {
      PartReaction reaction{.handled = true, .relayout = false, .requests = {}};
      if (request) {
        reaction.requests.push_back(*request);
      }
      return reaction;
    }

    // The Nest: the scene is laid out, textured and peeked into on workers (NestWork); this part keeps the peek
    // card, the dwell that opens it, and the keyboard's stops.
    class NestPart final : public ScenePart {
    public:
      NestPart()
          : m_work({
                .laidOut =
                    [this](std::optional<workbench::NestPeek> kept, bool same) { laidOut(std::move(kept), same); },
                .textured = [this] { redraw(0); },
                .peeked = [this](workbench::NestPeek peek) { opened(std::move(peek)); },
            }) {}

      void connect(const std::function<void()>& ready, const std::function<void(float)>& redraw) override {
        m_ready = ready;
        m_redraw = redraw;
      }

      bool layout(const PartFrame& frame) override {
        m_work.layout(frame, m_peek ? m_peek->node : workbench::kNoNode);
        return false;
      }

      workbench::SceneShapes shapes(const workbench::ShapeMarks& marks) const override;

      PartSnapshot shownSnapshot() const override {
        return {.tree = m_work.shown().tree, .serial = m_work.shown().serial};
      }

      // The core, inside the rings.
      std::optional<workbench::ShapeRect> noteBox(float width, float height) const override {
        const auto& scene = m_work.shown().scene;
        if (scene == nullptr) {
          return ScenePart::noteBox(width, height);
        }
        const auto f = [](double value) { return static_cast<float>(value); };
        const auto& core = scene->core;
        return workbench::ShapeRect{
            .x = f(core.x), .y = f(core.y), .width = f(core.w), .height = f(core.h), .radius = 0
        };
      }

      PartReaction wheel(const ShapeWheel& wheel) override;
      PartReaction key(const InputArea::KeyData& key, const workbench::ShapeMarks& marks) override;

      // A click closes the card and leaves its meaning to clickRequest.
      PartReaction activate(ShapeId /*id*/) override {
        closePeek();
        return {};
      }

      // Entries and the card's head describe their entry, and a "+N more" tile of the core or of a card its total when
      // that reads "at least"; ring segments, merged ring pieces and drawing details have none.
      TooltipContent tooltip(ShapeId id) const override {
        const auto& tree = m_work.shown().tree;
        if (tree == nullptr) {
          return {};
        }
        if (workbench::sceneRole(id.role) != SceneRole::More) {
          return m_tips.entry(tree, nullptr, id);
        }
        // The tile of the core, or the one of an open card, for the folder the shape names.
        const auto merged = [&id](const std::vector<workbench::NestPiece>& pieces) {
          return std::ranges::find_if(pieces, [&id](const workbench::NestPiece& piece) {
            const bool tile =
                piece.kind == workbench::NestPieceKind::More || piece.kind == workbench::NestPieceKind::PeekMore;
            return tile && piece.node == id.node;
          });
        };
        if (const auto* shown = scene(); shown != nullptr) {
          if (const auto more = merged(shown->pieces); more != shown->pieces.end()) {
            return m_tips.merged(tree, id, more->nodes);
          }
        }
        if (m_peek) {
          if (const auto more = merged(m_peek->pieces); more != m_peek->pieces.end()) {
            return m_tips.merged(tree, id, more->nodes);
          }
        }
        return {};
      }

      void hover(std::optional<ShapeId> id) override;

      float transitionMs() const override { return kTransitionMs; }

    private:
      const workbench::NestScene* scene() const { return m_work.shown().scene.get(); }
      std::optional<std::size_t> tileOf(NodeId node) const;
      std::optional<std::size_t> moreStop(const workbench::ShapeMarks& marks) const;
      bool onCard(std::optional<ShapeId> id) const;
      std::optional<PartReaction> peekKey(std::uint32_t sym);
      PartReaction plainKey(std::uint32_t sym, const workbench::ShapeMarks& marks);
      PartReaction stopAt(std::size_t piece, const workbench::ShapeMarks& marks);
      void ask(std::size_t piece, bool keyboard);
      void dwellEnded();
      void stopDwell();
      void closePeek();
      void laidOut(std::optional<workbench::NestPeek> kept, bool sameFolder);
      void opened(workbench::NestPeek peek);
      void redraw(float transitionMs) const {
        if (m_redraw) {
          m_redraw(transitionMs);
        }
      }

      NestWork m_work;
      std::function<void()> m_ready;
      std::function<void(float)> m_redraw;
      LevelWheel m_wheel;
      Timer m_dwell;
      NodeId m_dwellFor = workbench::kNoNode; // the tile the dwell runs for
      std::optional<ShapeId> m_hovered;
      std::optional<workbench::NestPeek> m_peek;
      NodeId m_peekCursor = workbench::kNoNode; // the keyboard's tile in the card
      bool m_keyboard = false;                  // the card was opened by a key
      bool m_asking = false;                    // a card is being built
      bool m_askKeyboard = false;
      bool m_moreStop = false; // the keyboard rests on "+N more" while the list's cursor stays m_cursorAtMore
      NodeId m_cursorAtMore = workbench::kNoNode;
      mutable EntryTooltips m_tips;
      bool m_quietHover = false; // the next hover change comes from the card going under a still pointer
    };

    workbench::SceneShapes NestPart::shapes(const workbench::ShapeMarks& marks) const {
      const auto& shown = m_work.shown();
      if (shown.tree == nullptr || shown.scene == nullptr) {
        return {};
      }
      auto sceneMarks = marks;
      if (const auto more = moreStop(marks)) {
        // The cursor's ring goes to the piece that holds a node: "+N more" holds its first entry.
        sceneMarks.cursor = shown.scene->pieces[*more].nodes.front();
      }
      auto out = workbench::nestShapes(*shown.tree, *shown.scene, shown.texture, sceneMarks, shown.palette);
      if (!m_peek) {
        return out;
      }
      const workbench::ShapeMarks cardMarks{
          .hovered = marks.hovered, .linked = marks.linked, .cursor = m_keyboard ? m_peekCursor : workbench::kNoNode
      };
      auto card = workbench::nestPeekShapes(*shown.tree, *m_peek, cardMarks, shown.palette);
      return workbench::nestWithCard(std::move(out), std::move(card), m_peek->card);
    }

    PartReaction NestPart::wheel(const ShapeWheel& wheel) {
      const auto& shown = m_work.shown();
      if (shown.tree == nullptr || scene() == nullptr) {
        return {};
      }
      // Over the card only its folder tiles count; the tiles beneath it are out of reach, and a turn up over its head
      // or a file leaves the card open.
      NodeId under = workbench::kNoNode;
      if (const auto at = m_peek ? workbench::nestPeekHit(*m_peek, wheel.x, wheel.y) : std::nullopt) {
        const auto* piece = *at < m_peek->pieces.size() ? &m_peek->pieces[*at] : nullptr;
        const bool folder = piece != nullptr && piece->kind == NestPieceKind::PeekTile && piece->folder;
        if (wheel.vertical && wheel.delta < 0 && !folder) {
          return handled();
        }
        under = folder ? piece->node : workbench::kNoNode;
      } else if (const auto piece = workbench::nestHit(*scene(), wheel.x, wheel.y)) {
        const auto& hit = scene()->pieces[*piece];
        under = hit.kind == NestPieceKind::Tile ? hit.node : workbench::kNoNode;
      }
      closePeek();
      return m_wheel.turn(wheel, *shown.tree, under, std::chrono::steady_clock::now());
    }

    PartReaction NestPart::key(const InputArea::KeyData& key, const workbench::ShapeMarks& marks) {
      if (scene() == nullptr || key.modifiers != 0) {
        return {};
      }
      if (m_peek && m_keyboard) {
        if (auto reaction = peekKey(key.sym)) {
          return *reaction;
        }
      }
      if (KeySymbol::isSpace(key.sym)) {
        // On "+N more" no tile holds the keyboard.
        const auto tile = moreStop(marks) ? std::nullopt : tileOf(marks.cursor);
        if (tile && scene()->pieces[*tile].folder) {
          closePeek();
          ask(*tile, true);
        }
        return handled();
      }
      closePeek();
      return plainKey(key.sym, marks);
    }

    // While a card opened by a key is open: arrows walk its tiles, Enter opens the one walked to (or the folder),
    // Space closes it. None for other keys.
    std::optional<PartReaction> NestPart::peekKey(std::uint32_t sym) {
      if (KeySymbol::isSpace(sym)) {
        closePeek();
        return handled();
      }
      if (const auto step = arrowStep(sym)) {
        std::vector<NodeId> nodes;
        std::vector<workbench::TreemapRect> boxes;
        std::optional<std::size_t> from;
        for (const auto& piece : m_peek->pieces) {
          if (piece.kind == NestPieceKind::PeekTile) {
            from = piece.node == m_peekCursor ? std::optional(nodes.size()) : from;
            nodes.push_back(piece.node);
            boxes.push_back(piece.rect);
          }
        }
        if (const auto next = workbench::nestNearest(boxes, from, step->dx, step->dy)) {
          m_peekCursor = nodes[*next];
          redraw(0);
        }
        return handled();
      }
      if (KeySymbol::isEnter(sym)) {
        const auto node = m_peekCursor != workbench::kNoNode ? m_peekCursor : m_peek->node;
        auto request = enterRequest(*m_work.shown().tree, node);
        closePeek();
        return handled(request);
      }
      return std::nullopt;
    }

    PartReaction NestPart::plainKey(std::uint32_t sym, const workbench::ShapeMarks& marks) {
      const auto& tree = *m_work.shown().tree;
      const auto stops = workbench::nestStops(*scene());
      if (const auto step = arrowStep(sym)) {
        std::vector<workbench::TreemapRect> boxes;
        std::optional<std::size_t> from;
        const auto more = moreStop(marks);
        for (const auto stop : stops) {
          const auto& piece = scene()->pieces[stop];
          const bool here = more ? stop == *more : piece.kind == NestPieceKind::Tile && piece.node == marks.cursor;
          from = here ? std::optional(boxes.size()) : from;
          boxes.push_back(piece.rect);
        }
        const auto next = workbench::nestNearest(boxes, from, step->dx, step->dy);
        return next ? stopAt(stops[*next], marks) : handled();
      }
      if ((KeySymbol::isHome(sym) || KeySymbol::isEnd(sym)) && !stops.empty()) {
        return stopAt(KeySymbol::isHome(sym) ? stops.front() : stops.back(), marks);
      }
      if (KeySymbol::isEnter(sym)) {
        if (moreStop(marks)) {
          return handled(PartRequest{.kind = PartRequestKind::ShowMore, .node = scene()->folder});
        }
        return handled(enterRequest(tree, marks.cursor));
      }
      if (KeySymbol::isBackspace(sym)) {
        return handled(upRequest(tree));
      }
      return {};
    }

    // Puts the keyboard on a stop: a tile takes the list's cursor, "+N more" holds it here.
    PartReaction NestPart::stopAt(std::size_t piece, const workbench::ShapeMarks& marks) {
      const auto& stop = scene()->pieces[piece];
      const bool more = stop.kind == NestPieceKind::More;
      const bool wasMore = moreStop(marks).has_value();
      m_moreStop = more;
      m_cursorAtMore = marks.cursor;
      if (more != wasMore) {
        redraw(0);
      }
      if (more) {
        return handled();
      }
      return handled(PartRequest{.kind = PartRequestKind::Select, .node = stop.node});
    }

    std::optional<std::size_t> NestPart::tileOf(NodeId node) const {
      if (scene() == nullptr || node == workbench::kNoNode) {
        return std::nullopt;
      }
      for (std::size_t i = 0; i < scene()->pieces.size(); ++i) {
        const auto& piece = scene()->pieces[i];
        if (piece.kind == NestPieceKind::Tile && piece.node == node) {
          return i;
        }
      }
      return std::nullopt;
    }

    std::optional<std::size_t> NestPart::moreStop(const workbench::ShapeMarks& marks) const {
      if (!m_moreStop || marks.cursor != m_cursorAtMore || scene() == nullptr) {
        return std::nullopt;
      }
      const auto& pieces = scene()->pieces;
      const auto more = std::ranges::find_if(pieces, [](const workbench::NestPiece& piece) {
        return piece.kind == NestPieceKind::More && !piece.nodes.empty();
      });
      return more == pieces.end() ? std::nullopt : std::optional(static_cast<std::size_t>(more - pieces.begin()));
    }

    bool NestPart::onCard(std::optional<ShapeId> id) const {
      if (!m_peek || !id) {
        return false;
      }
      const auto role = workbench::sceneRole(id->role);
      if (role == SceneRole::Head || role == SceneRole::More) {
        return id->node == m_peek->node;
      }
      return role == SceneRole::Piece && std::ranges::any_of(m_peek->pieces, [&id](const workbench::NestPiece& piece) {
               return piece.kind == NestPieceKind::PeekTile && piece.node == id->node;
             });
    }

    // The pointer resting on a folder tile for the dwell opens its card; moving off the card closes it.
    void NestPart::hover(std::optional<ShapeId> id) {
      m_hovered = id;
      if (m_peek && !onCard(id)) {
        closePeek();
      }
      if (std::exchange(m_quietHover, false)) {
        stopDwell();
        return;
      }
      std::optional<NodeId> tile;
      if (id && workbench::sceneRole(id->role) == SceneRole::Piece && !m_peek) {
        const auto piece = tileOf(id->node);
        const auto& tree = m_work.shown().tree;
        if (piece && tree != nullptr && workbench::nestPeekAllowed(*tree, *scene(), *piece)) {
          tile = id->node;
        }
      }
      if (tile && *tile == m_dwellFor) {
        return;
      }
      stopDwell();
      if (tile) {
        m_dwellFor = *tile;
        m_dwell.start(kDwell, [this] { dwellEnded(); });
      }
    }

    void NestPart::dwellEnded() {
      const auto node = std::exchange(m_dwellFor, workbench::kNoNode);
      const bool still =
          m_hovered && m_hovered->node == node && workbench::sceneRole(m_hovered->role) == SceneRole::Piece;
      if (const auto piece = still ? tileOf(node) : std::nullopt) {
        ask(*piece, false);
      }
    }

    // Stops the dwell, and forgets a card the pointer asked for.
    void NestPart::stopDwell() {
      m_dwell.stop();
      m_dwellFor = workbench::kNoNode;
      if (m_asking && !m_askKeyboard) {
        m_asking = false;
        m_work.dropPeek();
      }
    }

    void NestPart::ask(std::size_t piece, bool keyboard) {
      m_asking = true;
      m_askKeyboard = keyboard;
      m_work.peek(piece);
    }

    void NestPart::opened(workbench::NestPeek peek) {
      m_asking = false;
      m_peek = std::move(peek);
      m_keyboard = m_askKeyboard;
      m_peekCursor = workbench::kNoNode;
      redraw(static_cast<float>(workbench::kNestPeekOpenMs));
    }

    // Closes the card. A pointer resting on it then lies on a tile it did not move to, which starts no dwell.
    void NestPart::closePeek() {
      if (m_asking) {
        m_asking = false;
        m_work.dropPeek();
      }
      if (!m_peek) {
        return;
      }
      m_quietHover = onCard(m_hovered);
      m_peek.reset();
      m_keyboard = false;
      m_peekCursor = workbench::kNoNode;
      redraw(0);
    }

    // A new scene keeps the card when it still shows its folder, rebuilt for it, and the "+N more" stop when it shows
    // the same current folder.
    void NestPart::laidOut(std::optional<workbench::NestPeek> kept, bool sameFolder) {
      m_asking = false;
      m_moreStop = m_moreStop && sameFolder;
      if (m_peek && kept) {
        const bool cursorKept = std::ranges::any_of(kept->pieces, [this](const workbench::NestPiece& piece) {
          return piece.kind == NestPieceKind::PeekTile && piece.node == m_peekCursor;
        });
        m_peekCursor = cursorKept ? m_peekCursor : workbench::kNoNode;
        m_peek = std::move(kept);
      } else {
        m_peek.reset();
        m_keyboard = false;
        m_peekCursor = workbench::kNoNode;
      }
      if (m_ready) {
        m_ready();
      }
    }
  } // namespace

  std::unique_ptr<ScenePart> makeNestPart() { return std::make_unique<NestPart>(); }
} // namespace luciole
