#include "ui/controls/text_tile_view.h"

#include "formats/hex_dump.h"
#include "render/core/renderer.h"
#include "ui/controls/box.h"
#include "ui/controls/label.h"
#include "ui/controls/scrollbar.h"
#include "ui/controls/text_tile_view_style.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <wayland-client-protocol.h>

namespace {
  using luciole::TextDocumentState;
  namespace formats = luciole::formats;
  namespace view = luciole::text_tile_view;

  constexpr float kCodeScale = 0.95F; // code beside prose reads a little smaller
  constexpr std::size_t kUploadsPerUpdate = 2;

  struct Overflow {
    float x = 0;
    float y = 0;
  };

  // How far content scrolls each way in a viewport: nothing when only its trailing `margin` is out of sight, unless
  // the other way's bar lies over the text's edge.
  Overflow overflow(const luciole::TextTileGeometry& content, double scale, float margin, float width, float height) {
    const float pastX = static_cast<float>(content.width / scale) - width;
    const float pastY = static_cast<float>(content.height / scale) - height;
    const float bar = Style::scrollbarWidth;
    bool across = pastX > margin;
    const bool down = pastY > margin - (across ? bar : 0.0F);
    across = pastX > margin - (down ? bar : 0.0F);
    return {.x = across ? pastX : 0.0F, .y = down ? pastY : 0.0F};
  }

  using view::colorText, view::paletteTheme, view::wordsOf, view::wraps;
} // namespace

TextTileView::TextTileView(luciole::TextTileService& service, std::size_t textureBudget)
    : m_service(service), m_budget(textureBudget) {
  setClipChildren(true);
  m_background = static_cast<Box*>(addChild(std::make_unique<Box>()));
  m_background->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
  m_layer = addChild(std::make_unique<Node>());
  m_layer->setClipChildren(true);
  m_layer->setHitTestVisible(false);
  m_tiles = std::make_unique<view::TileTextures>(*m_layer);
  m_area = static_cast<InputArea*>(addChild(std::make_unique<InputArea>()));
  m_area->setOnAxisHandler([this](const InputArea::PointerData& pointer) {
    const float delta = pointer.scrollDelta(Style::scrollWheelStep);
    if (pointer.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
      scrollTo(m_scrollX + delta, m_scrollY);
      return maxScrollX() > 0;
    }
    scrollTo(m_scrollX, m_scrollY + delta);
    return maxScrollY() > 0;
  });
  m_area->setOnEnter([this](const InputArea::PointerData& pointer) { hover(pointer.localX, pointer.localY); });
  m_area->setOnMotion([this](const InputArea::PointerData& pointer) { hover(pointer.localX, pointer.localY); });
  m_area->setOnLeave([this] { hover(-1, -1); });
  m_area->setTooltipProvider([this] -> TooltipContent {
    return m_hovered ? TooltipContent(colorText(*m_hovered)) : TooltipContent{};
  });
  m_label = static_cast<Label*>(addChild(std::make_unique<Label>()));
  m_label->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_verticalBar = static_cast<Scrollbar*>(addChild(std::make_unique<Scrollbar>()));
  m_verticalBar->setOnScrollChanged([this](float offset) { scrollTo(m_scrollX, offset); });
  m_horizontalBar = static_cast<Scrollbar*>(addChild(std::make_unique<Scrollbar>()));
  m_horizontalBar->setOrientation(ScrollOrientation::Horizontal);
  m_horizontalBar->setOnScrollChanged([this](float offset) { scrollTo(offset, m_scrollY); });
  m_paletteConn = paletteChanged().connect([this] {
    ++m_palette;
    needFrame();
  });
}

TextTileView::~TextTileView() {
  m_relayoutTimer.stop();
  m_hexToken.cancel();
  m_tiles->clear();
}

void TextTileView::reset() {
  m_relayoutTimer.stop();
  m_handle.close();
  m_tiles->clear();
  m_arrived.clear();
  m_asked.clear();
  m_broken.clear();
  m_requested.reset();
  m_pending.reset();
  m_revisionBase = 0;
  m_target = {};
  m_shown = {};
  m_failed = false;
  m_hovered.reset();
  m_hexToken.cancel();
  m_hexToken = luciole::CancelToken{};
  m_hexAsked = 0;
  m_hexReady.reset();
  markLayoutDirty();
}

void TextTileView::showLoading() {
  show(luciole::TextDocument{});
  m_words = "Loading…";
}

void TextTileView::show(const luciole::TextDocument& document) {
  reset();
  m_source = document;
  if (m_source.state == TextDocumentState::Shown && m_source.facts.bytes == 0) {
    m_source.document.reset(); // the words say it all
  }
  m_wraps = m_source.document && wraps(*m_source.document);
  const bool hex = m_source.state == TextDocumentState::NotText && m_source.document;
  m_hexRows = hex ? formats::hexDumpRowBytes(*m_source.document) : 0;
  m_hexAdvance = 0;
  std::tie(m_words, m_caption) = wordsOf(m_source);
  m_scrollX = 0;
  m_scrollY = 0;
  needFrame();
}

void TextTileView::setTextSize(float size) {
  const float clamped = std::isnan(size) ? kDefaultTextSize : std::clamp(size, kMinTextSize, kMaxTextSize);
  if (clamped != m_textSize) {
    m_textSize = clamped;
    markLayoutDirty();
    needFrame();
  }
}

void TextTileView::setShown(bool shown) {
  if (shown == visible()) {
    return;
  }
  setVisible(shown);
  if (!shown) {
    reset(); // keeps the document and its words for when it is shown again
    return;
  }
  needFrame();
}

void TextTileView::scrollTo(float x, float y) {
  const float clampedX = std::isnan(x) ? 0.0F : std::clamp(x, 0.0F, maxScrollX());
  const float clampedY = std::isnan(y) ? 0.0F : std::clamp(y, 0.0F, maxScrollY());
  if (clampedX == m_scrollX && clampedY == m_scrollY) {
    return;
  }
  m_scrollX = clampedX;
  m_scrollY = clampedY;
  markLayoutDirty();
  needFrame();
}

float TextTileView::maxScrollX() const noexcept {
  return overflow(m_shown.geometry, m_shown.scale, kMargin, m_viewportWidth, m_viewportHeight).x;
}

float TextTileView::maxScrollY() const noexcept {
  return overflow(m_shown.geometry, m_shown.scale, kMargin, m_viewportWidth, m_viewportHeight).y;
}

std::size_t TextTileView::textureBytes() const noexcept { return m_tiles->bytes(); }

std::size_t TextTileView::residentTiles() const noexcept { return m_tiles->count(); }

std::size_t TextTileView::waitingBytes() const noexcept {
  const auto bytes = [](const auto& entry) { return entry.second.rgba.size(); };
  return std::ranges::fold_left(m_arrived | std::views::transform(bytes), std::size_t{0}, std::plus{});
}

bool TextTileView::settled() const {
  if (!visible() || !m_source.document || m_failed) {
    return true;
  }
  const bool hexWaits = m_hexAsked != 0 || hexRowsWanted() != m_hexRows;
  if (!m_requested || m_pending || desiredKey() != *m_requested || m_shown.revision == 0 || hexWaits) {
    return false;
  }
  if (m_shown.revision != m_target.revision || m_shown.revision != latestRevision()) {
    return false;
  }
  const auto wanted = window(m_shown);
  return std::ranges::all_of(wanted.tiles, [&](std::size_t index) {
    return m_tiles->holds(index) || m_broken.contains(index);
  });
}

TextTileView::Key TextTileView::desiredKey() const {
  return {.width = m_wraps ? m_viewportWidth : 0.0, .scale = m_scale, .textSize = m_textSize, .palette = m_palette};
}

std::uint64_t TextTileView::latestRevision() const { return m_revisionBase + m_handle.revision(); }

view::TileWindow TextTileView::window(const Laid& laid) const {
  return view::tileWindow(laid.geometry, scrollYFor(laid) * laid.scale, m_viewportHeight * laid.scale, m_budget);
}

void TextTileView::open(const Key& key) {
  luciole::TextTileOptions options{
      .width = key.width,
      .scale = key.scale,
      .theme = paletteTheme(),
      .fonts = {.proseSize = key.textSize, .codeSize = key.textSize * kCodeScale},
  };
  if (m_handle.valid()) {
    m_handle.relayout(options);
  } else {
    m_handle = m_service.open(
        m_source.document, options,
        {
            .laidOut = [this, base = m_revisionBase](
                           std::uint64_t revision, auto geometry
                       ) { laidOut(base + revision, std::move(geometry)); },
            .tile = [this, base = m_revisionBase](
                        std::uint64_t revision, std::size_t index, auto tile
                    ) { arrived(base + revision, index, std::move(tile)); },
        }
    );
  }
  m_requested = key;
}

void TextTileView::relayout() {
  if (!m_pending || !m_handle.valid()) {
    return;
  }
  const auto wanted = *std::exchange(m_pending, std::nullopt);
  if (wanted.key != *m_requested) {
    open(wanted.key);
  }
  (void)refitHex(wanted.hexRows);
}

void TextTileView::laidOut(std::uint64_t revision, std::expected<luciole::TextTileGeometry, std::string> geometry) {
  if (!geometry) {
    m_failed = true;
    m_tiles->clear();
    m_words = "This text could not be drawn.";
    m_caption = false;
    markLayoutDirty();
    return;
  }
  if (m_hexRows > 0 && m_requested) {
    const double width = (geometry->width / m_requested->scale) - (2 * kMargin);
    m_hexAdvance = static_cast<float>(width / static_cast<double>(formats::hexDumpRowWidth(m_hexRows)));
    if (m_shown.revision == 0 && refitHex(hexRowsWanted())) {
      return; // nothing is on screen yet, so the dump whose rows fit comes first
    }
  }
  m_target = {
      .revision = revision,
      .geometry = *geometry,
      .scale = m_requested ? m_requested->scale : 1.0,
      .hexRows = m_hexRows,
      .textSize = m_requested ? m_requested->textSize : m_textSize,
      .width = m_requested ? m_requested->width : 0.0,
  };
  m_arrived.clear();
  m_asked.clear();
  m_broken.clear();
  needFrame();
}

void TextTileView::arrived(
    std::uint64_t revision, std::size_t index, std::expected<luciole::TextTile, std::string> tile
) {
  if (revision != m_target.revision) {
    return;
  }
  m_asked.erase(index);
  if (!tile) {
    m_broken.insert(index);
  } else {
    m_arrived[index] = std::move(*tile);
  }
  needFrame();
}

void TextTileView::update() {
  if (!visible() || !m_source.document || m_failed || m_textures == nullptr || m_viewportWidth <= 0) {
    return;
  }
  const Wanted wanted{.key = desiredKey(), .hexRows = hexRowsWanted()};
  if (!m_handle.valid()) {
    if (!refitHex(wanted.hexRows)) {
      open(wanted.key);
    }
  } else if (wanted != Wanted{.key = *m_requested, .hexRows = m_hexAsked != 0 ? m_hexAsked : m_hexRows}) {
    if (m_pending != wanted) {
      m_pending = wanted;
      m_relayoutTimer.start(kRelayoutDelay, [this] {
        relayout();
        needFrame();
      });
    }
  } else if (m_pending) {
    m_pending.reset();
    m_relayoutTimer.stop();
  }
  swapWhenCovered();
  requestMissing();
  uploadArrived();
  openHexWhenCovered();
}

void TextTileView::swapWhenCovered() {
  if (m_target.revision == 0 || m_target.revision == m_shown.revision) {
    return;
  }
  const auto wanted = window(m_target);
  const auto visibleTiles = std::span(wanted.tiles).first(wanted.visible);
  const bool covered = std::ranges::all_of(visibleTiles, [&](std::size_t index) {
    return m_arrived.contains(index) || m_broken.contains(index);
  });
  if (!covered) {
    return;
  }
  m_tiles->clear();
  m_scrollY = scrollYFor(m_target);
  m_shown = m_target;
  if (!m_caption && m_shown.geometry.truncated) {
    m_words = "Preview truncated";
    m_caption = true;
  }
  for (const auto index : visibleTiles) {
    if (const auto found = m_arrived.find(index); found != m_arrived.end()) {
      m_tiles->upload(*m_textures, index, found->second);
      m_arrived.erase(found);
    }
  }
  scrollTo(m_scrollX, m_scrollY);
  markLayoutDirty();
}

void TextTileView::requestMissing() {
  if (m_target.revision <= m_revisionBase) {
    return; // none, or a layout of a document closed for a hex dump's new rows
  }
  const bool onScreen = m_shown.revision == m_target.revision;
  const auto wanted = window(m_target);
  std::erase_if(m_arrived, [&](const auto& entry) { return !std::ranges::contains(wanted.tiles, entry.first); });
  std::vector<std::size_t> missing;
  for (const auto index : wanted.tiles) {
    if (!m_arrived.contains(index) && !m_broken.contains(index) && !(onScreen && m_tiles->holds(index))) {
      missing.push_back(index);
    }
  }
  if (std::ranges::all_of(missing, [&](std::size_t index) { return m_asked.contains(index); })) {
    return; // already queued or being drawn
  }
  m_handle.request(missing);
  m_asked = {missing.begin(), missing.end()};
}

void TextTileView::uploadArrived() {
  if (m_shown.revision == 0 || m_shown.revision != m_target.revision) {
    return;
  }
  const auto wanted = window(m_shown); // requestMissing kept only these in m_arrived
  m_tiles->keepOnly(wanted.tiles);
  std::size_t uploaded = 0;
  for (const auto index : wanted.tiles) {
    const auto found = m_arrived.find(index);
    if (found == m_arrived.end()) {
      continue;
    }
    if (uploaded == kUploadsPerUpdate) {
      needFrame(); // the rest go up in the next frames
      return;
    }
    m_tiles->upload(*m_textures, index, found->second);
    m_arrived.erase(found);
    ++uploaded;
  }
  if (uploaded > 0) {
    markLayoutDirty();
  }
}

void TextTileView::hover(float x, float y) {
  std::optional<luciole::formats::Rgba> found;
  if (x >= 0 && y >= 0 && m_shown.revision != 0) {
    const double s = m_shown.scale;
    found = m_tiles->swatchAt(std::round(m_scrollX * s) + x * s, std::round(m_scrollY * s) + y * s);
  }
  if (found != m_hovered) {
    m_hovered = found;
    m_area->requestTooltipRefresh();
  }
}

void TextTileView::needFrame() {
  if (onFrameNeeded && visible()) {
    onFrameNeeded();
  }
}

void TextTileView::doLayout(Renderer& renderer) {
  if (!visible()) {
    return;
  }
  m_textures = &renderer.textureManager();
  const float scale = renderer.renderScale();
  const double pixelScale = std::isfinite(scale) && scale > 0 ? scale : 1.0;
  const float w = width();
  const float h = height();
  m_background->setFrameSize(w, h);
  layoutWords(renderer, w, h);
  const float top = m_label->visible() && m_caption ? m_label->y() + m_label->height() + Style::spaceXs : 0.0F;
  const auto snappedTop = static_cast<float>(std::ceil(top * pixelScale) / pixelScale);
  const float viewportHeight = std::max(0.0F, h - snappedTop);
  if (w != m_viewportWidth || viewportHeight != m_viewportHeight || pixelScale != m_scale) {
    m_viewportWidth = w;
    m_viewportHeight = viewportHeight;
    m_scale = pixelScale;
    needFrame();
  }
  if (m_shown.revision != 0) { // before a layout is shown, the scroll position waits for it
    m_scrollX = std::min(m_scrollX, maxScrollX());
    m_scrollY = std::min(m_scrollY, maxScrollY());
  }
  m_layer->setPosition(0, snappedTop);
  m_layer->setFrameSize(w, viewportHeight);
  m_area->setPosition(0, snappedTop);
  m_area->setFrameSize(w, viewportHeight);
  m_tiles->place(m_shown.scale, m_scrollX, m_scrollY);
  layoutBars(renderer, w, h);
}

void TextTileView::layoutWords(Renderer& renderer, float width, float height) {
  m_label->setVisible(!m_words.empty());
  if (m_words.empty()) {
    return;
  }
  const float pad = Style::spaceSm;
  const float room = std::max(0.0F, width - (2 * pad));
  m_label->setText(m_words);
  m_label->setFontSize(m_textSize);
  m_label->setMaxLines(m_caption ? 1 : 0);
  m_label->setMaxWidth(room);
  const auto size = m_label->Node::measure(renderer, LayoutConstraints::available(room, height));
  m_label->arrange(renderer, {pad, pad, size.width, size.height});
}

void TextTileView::layoutBars(Renderer& renderer, float width, float height) {
  const float top = m_layer->y();
  const float thickness = Style::scrollbarWidth;
  const auto contentWidth = static_cast<float>(m_shown.geometry.width / m_shown.scale);
  const auto contentHeight = static_cast<float>(m_shown.geometry.height / m_shown.scale);
  const bool vertical = maxScrollY() > 0;
  const bool horizontal = maxScrollX() > 0;
  m_verticalBar->setVisible(vertical);
  m_horizontalBar->setVisible(horizontal);
  if (vertical) {
    m_verticalBar->update(m_viewportHeight, contentHeight, m_scrollY);
    m_verticalBar->arrange(
        renderer, {width - thickness, top, thickness, m_viewportHeight - (horizontal ? thickness : 0.0F)}
    );
  }
  if (horizontal) {
    m_horizontalBar->update(m_viewportWidth, contentWidth, m_scrollX);
    m_horizontalBar->arrange(
        renderer, {0, height - thickness, m_viewportWidth - (vertical ? thickness : 0.0F), thickness}
    );
  }
}

void TextTileView::doInvalidateGpuResources(Renderer& /*renderer*/) {
  m_tiles->forget();
  m_asked.clear();
  markLayoutDirty();
  needFrame();
}
