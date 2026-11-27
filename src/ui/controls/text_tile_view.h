#pragma once

#include "core/timer_manager.h"
#include "fs/cancel_token.h"
#include "imaging/text_document_loader.h"
#include "imaging/text_tile_service.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/controls/text_tile_view_tiles.h"
#include "ui/signal.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

class Box;
class Label;
class Scrollbar;

// A text document drawn as colored tiles on the service's worker, shown in a scrollable area. Code keeps its lines
// and scrolls sideways; prose wraps to the view's width. The tiles crossing the viewport hold textures, with one more
// each way within a byte budget, and a hidden or destroyed view holds none but keeps its scroll position. A new width,
// pixel scale, palette or text size lays the document out again after kRelayoutDelay, while the old tiles stay on
// screen until the new ones cover the viewport; meanwhile only the new tiles near the viewport are kept. A hex dump
// takes the most bytes per row, of 16, 12, 8 and 4, whose rows fit the width: when the width or the text calls for
// others, after the same delay, its rows are made again on a worker and laid out the same way, keeping the same
// bytes in view. Hovering a color swatch shows its value.
// Loading, cut text and refusals are said in words.
//
// The owner calls update() once per frame before layout; it asks for tiles, uploads the ones that arrived and
// starts layouts. onFrameNeeded asks for that frame.
class TextTileView final : public Node {
public:
  static constexpr std::size_t kDefaultTextureBudget = 32U * 1024U * 1024U;
  static constexpr std::chrono::milliseconds kRelayoutDelay{120};
  static constexpr float kDefaultTextSize = 12.0F;
  static constexpr float kMinTextSize = 6.0F;
  static constexpr float kMaxTextSize = 48.0F;

  // The budget limits the tiles kept beside the viewport; the tiles it shows always hold textures.
  explicit TextTileView(luciole::TextTileService& service, std::size_t textureBudget = kDefaultTextureBudget);
  ~TextTileView() override;
  TextTileView(const TextTileView&) = delete;
  TextTileView& operator=(const TextTileView&) = delete;

  void showLoading();
  // Shows the document of a loader's result, or the words of its state.
  void show(const luciole::TextDocument& document);
  // Logical pixels of the prose; code is a little smaller. Clamped to [kMinTextSize, kMaxTextSize]. The line at the
  // top stays there once the new layout shows.
  void setTextSize(float size);
  float textSize() const noexcept { return m_textSize; }
  // Hiding releases every texture, the layout on the worker and the timer; showing lays the document out again at
  // the same scroll position.
  void setShown(bool shown);
  void update();
  // Logical pixels, clamped to the content.
  void scrollTo(float x, float y);

  float scrollX() const noexcept { return m_scrollX; }
  float scrollY() const noexcept { return m_scrollY; }
  // 0 while all the text is in sight and clear of the other bar, even when the empty margin after it is not; a bar
  // shows only past 0.
  float maxScrollX() const noexcept;
  float maxScrollY() const noexcept;
  // The height the words and the text laid out take, in logical pixels, at `width`: what a card around the view
  // needs to show it whole. Text still being laid out counts as it stands on screen.
  float naturalHeight(Renderer& renderer, float width);
  // The words shown: a state alone, or a caption above the text; empty for none.
  const std::string& message() const noexcept { return m_words; }
  std::size_t textureBytes() const noexcept;
  std::size_t residentTiles() const noexcept;
  // Bytes of the tiles that arrived and wait to be uploaded.
  std::size_t waitingBytes() const noexcept;
  // The revision and size of the layout on screen; 0 and empty before the first.
  std::uint64_t layoutRevision() const noexcept { return m_shown.revision; }
  const luciole::TextTileGeometry& geometry() const noexcept { return m_shown.geometry; }
  // A dump made again for another width has arrived and waits for the tiles on screen to be covered.
  bool hexAnswerHeld() const noexcept { return m_hexReady.has_value(); }
  // Nothing waits: the latest layout is on screen and every tile the viewport shows is uploaded.
  bool settled() const;

  InputArea& inputArea() const noexcept { return *m_area; }
  const Scrollbar& verticalBar() const noexcept { return *m_verticalBar; }
  const Scrollbar& horizontalBar() const noexcept { return *m_horizontalBar; }

  std::function<void()> onFrameNeeded;

protected:
  void doLayout(Renderer& renderer) override;
  void doInvalidateGpuResources(Renderer& renderer) override;

private:
  static constexpr float kMargin = 8.0F; // logical pixels the tile renderer keeps around a document

  // What a layout depends on; width is 0 for text that does not wrap.
  struct Key {
    double width = 0;
    double scale = 1;
    float textSize = kDefaultTextSize;
    std::uint64_t palette = 0;
    bool operator==(const Key&) const = default;
  };

  struct Laid {
    std::uint64_t revision = 0;
    luciole::TextTileGeometry geometry{};
    double scale = 1;
    std::size_t hexRows = 0; // bytes per row of the dump laid out; 0 for other text
    float textSize = kDefaultTextSize;
    double width = 0; // the wrap width asked for, in logical pixels; 0 for text that does not wrap
  };

  // A layout and the rows of a hex dump, as wanted or as asked for.
  struct Wanted {
    Key key;
    std::size_t hexRows = 0;
    bool operator==(const Wanted&) const = default;
  };

  Key desiredKey() const;
  // The revision of the latest layout asked for, counted across the documents a hex dump's rows are made into.
  std::uint64_t latestRevision() const;
  luciole::text_tile_view::TileWindow window(const Laid& laid) const;
  // The scroll position that shows `laid` at the bytes on screen.
  float scrollYFor(const Laid& laid) const;
  void reset();
  void open(const Key& key);
  void relayout();
  void laidOut(std::uint64_t revision, std::expected<luciole::TextTileGeometry, std::string> geometry);
  void arrived(std::uint64_t revision, std::size_t index, std::expected<luciole::TextTile, std::string> tile);
  void swapWhenCovered();
  void requestMissing();
  void uploadArrived();
  void hover(float x, float y);
  void needFrame();
  void layoutWords(Renderer& renderer, float width, float height);
  // Sets the words' label for `width` and measures it, within `height` when given.
  LayoutSize fitWords(Renderer& renderer, float width, std::optional<float> height);
  void layoutBars(Renderer& renderer, float width, float height);
  std::size_t hexRowsWanted() const;
  // Starts making the dump again in rows of `rows` bytes, or stops making it when those are the rows of the
  // document; true while other rows are being made.
  bool refitHex(std::size_t rows);
  void hexArrived(std::size_t rows, std::optional<luciole::formats::StyledDocument> dump);
  // Opens the dump that arrived once the visible tiles of the layout on screen are in, so the tiles of the old layout
  // that were asked for are not dropped with it.
  void openHexWhenCovered();
  bool shownCovered() const;

  luciole::TextTileService& m_service;
  std::size_t m_budget;
  luciole::TextTileDocument m_handle;
  luciole::TextDocument m_source;
  bool m_wraps = false;
  std::string m_words;
  bool m_caption = false; // the words sit above the text

  Box* m_background = nullptr;
  Node* m_layer = nullptr;
  InputArea* m_area = nullptr;
  Label* m_label = nullptr;
  Scrollbar* m_verticalBar = nullptr;
  Scrollbar* m_horizontalBar = nullptr;
  std::unique_ptr<luciole::text_tile_view::TileTextures> m_tiles;
  TextureManager* m_textures = nullptr;

  std::optional<Key> m_requested;                     // the key of the latest revision asked for
  std::optional<Wanted> m_pending;                    // what waits for the timer
  std::uint64_t m_revisionBase = 0;                   // added to the revisions of the open document
  Laid m_target;                                      // the latest layout made
  Laid m_shown;                                       // the layout on screen
  std::map<std::size_t, luciole::TextTile> m_arrived; // tiles of m_target's window not uploaded yet
  std::set<std::size_t> m_asked;                      // asked for and not arrived
  std::set<std::size_t> m_broken;                     // tiles of m_target that could not be drawn
  bool m_failed = false;                              // the layout could not be made

  Timer m_relayoutTimer;
  Signal<>::ScopedConnection m_paletteConn;
  std::uint64_t m_palette = 0;
  float m_textSize = kDefaultTextSize;
  float m_scrollX = 0;
  float m_scrollY = 0;
  float m_viewportWidth = 0;
  float m_viewportHeight = 0;
  double m_scale = 1;
  std::optional<luciole::formats::Rgba> m_hovered;

  // A hex dump: bytes per row of the open document (0 for other text), the logical width of a character in its
  // last layout, and the rows of the dump being made on the worker (0 for none).
  std::size_t m_hexRows = 0;
  float m_hexAdvance = 0;
  std::size_t m_hexAsked = 0;
  luciole::CancelToken m_hexToken; // cancelled when the dump goes or the view does, so no answer reaches either
  struct HexAnswer {
    std::size_t rows;
    luciole::formats::StyledDocument dump;
  };
  std::optional<HexAnswer> m_hexReady; // the dump that arrived and waits for the viewport to be covered
};
