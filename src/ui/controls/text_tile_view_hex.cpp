#include "formats/hex_dump.h"
#include "fs/background_job.h"
#include "ui/controls/text_tile_view.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

// The hex half of the text tile view: a dump's rows fitted to the width, made again on a worker.
namespace formats = luciole::formats;

std::size_t TextTileView::hexRowsWanted() const {
  if (m_hexRows == 0 || m_hexAdvance <= 0) {
    return m_hexRows;
  }
  return formats::hexDumpRowsFitting((m_viewportWidth - (2 * kMargin)) / m_hexAdvance);
}

namespace {
  // The device pixels from the top of line `line`'s row to the next row's top, or to the end.
  std::pair<double, double> rowOf(const std::vector<int>& tops, std::size_t line, int height) {
    const auto next = std::upper_bound(tops.begin() + static_cast<std::ptrdiff_t>(line), tops.end(), tops[line]);
    return {tops[line], next == tops.end() ? height : *next};
  }

  // The scroll position in `to` that shows the line at the top of `from`, scrolled to `scrollY`, at the same place in
  // its row. Logical pixels; above the first line, the margin stays as it is.
  float sameLineScrollY(
      const luciole::TextTileGeometry& from, double fromScale, float scrollY, const luciole::TextTileGeometry& to,
      double toScale
  ) {
    const double y = scrollY * fromScale;
    if (from.lineTops.empty() || to.lineTops.empty() || y < from.lineTops.front()) {
      return scrollY;
    }
    const auto line = static_cast<std::size_t>(std::ranges::upper_bound(from.lineTops, y) - from.lineTops.begin()) - 1;
    if (line >= to.lineTops.size()) {
      return static_cast<float>(to.height / toScale); // cut before that line: the end
    }
    const auto [top, bottom] = rowOf(from.lineTops, line, from.height);
    const double into = bottom > top ? std::clamp((y - top) / (bottom - top), 0.0, 1.0) : 0.0;
    const auto [newTop, newBottom] = rowOf(to.lineTops, line, to.height);
    return static_cast<float>((newTop + (into * (newBottom - newTop))) / toScale);
  }
} // namespace

float TextTileView::scrollYFor(const Laid& laid) const {
  const bool sizeChanged = laid.textSize != m_shown.textSize;
  if (m_shown.revision != 0 && (sizeChanged || laid.width != m_shown.width) && laid.hexRows == m_shown.hexRows) {
    // Another text size, or another width for text that wraps: the line on top stays there.
    return sameLineScrollY(m_shown.geometry, m_shown.scale, m_scrollY, laid.geometry, laid.scale);
  }
  if (m_shown.revision != 0 && sizeChanged) {
    // A dump's rows made again at another text size: its rows are alike, so the offset past the top margin follows
    // the text's height and the same bytes stay on top.
    const double before = (m_shown.geometry.height / m_shown.scale) - (2 * kMargin);
    const double after = (laid.geometry.height / laid.scale) - (2 * kMargin);
    if (before > 0 && after > 0) {
      return std::max(0.0F, kMargin + static_cast<float>((m_scrollY - kMargin) * after / before));
    }
  }
  if (laid.hexRows == 0 || m_shown.hexRows == 0 || laid.hexRows == m_shown.hexRows) {
    return m_scrollY;
  }
  return m_scrollY * static_cast<float>(m_shown.hexRows) / static_cast<float>(laid.hexRows); // the same bytes
}

bool TextTileView::refitHex(std::size_t rows) {
  if (rows == m_hexRows) {
    m_hexToken.cancel();
    m_hexAsked = 0;
    m_hexReady.reset();
    return false;
  }
  if (rows == m_hexAsked) {
    return true;
  }
  m_hexToken.cancel();
  m_hexToken = luciole::CancelToken{};
  m_hexReady.reset();
  m_hexAsked = rows;
  luciole::runJob<std::optional<formats::StyledDocument>>(
      m_hexToken, [dump = m_source.document, rows] { return formats::hexDumpAgain(*dump, rows); },
      [this, token = m_hexToken, rows](auto dump) {
        if (!token.cancelled()) {
          hexArrived(rows, dump ? std::move(*dump) : std::nullopt);
        }
      }
  );
  return true;
}

// The new rows open as a document of their own, whose layouts take revisions after the old one's, so the old tiles
// stay on screen until the new ones cover the viewport. The old document stays open until its own visible tiles are
// in, since closing it drops the tiles it was asked for.
void TextTileView::hexArrived(std::size_t rows, std::optional<formats::StyledDocument> dump) {
  if (!dump) {
    m_hexAsked = 0;
    m_hexRows = 0; // not a dump after all: it shows as it is
    open(m_requested.value_or(desiredKey()));
    return;
  }
  m_hexReady = HexAnswer{.rows = rows, .dump = std::move(*dump)};
  openHexWhenCovered();
  needFrame();
}

bool TextTileView::shownCovered() const {
  if (m_shown.revision == 0) {
    return true;
  }
  const auto wanted = window(m_shown);
  const bool brokenAreShown = m_target.revision == m_shown.revision; // m_broken belongs to the target layout
  return std::ranges::all_of(std::span(wanted.tiles).first(wanted.visible), [&](std::size_t index) {
    return m_tiles->holds(index) || (brokenAreShown && m_broken.contains(index));
  });
}

void TextTileView::openHexWhenCovered() {
  if (!m_hexReady || !shownCovered()) {
    return;
  }
  auto answer = std::move(*m_hexReady);
  m_hexReady.reset();
  m_hexAsked = 0;
  if (m_shown.revision == 0) {
    m_scrollY = m_scrollY * static_cast<float>(m_hexRows) / static_cast<float>(answer.rows); // the same bytes
  }
  m_revisionBase = latestRevision();
  m_handle.close();
  m_target = m_shown;
  m_arrived.clear();
  m_asked.clear();
  m_broken.clear();
  m_source.document = std::make_shared<const formats::StyledDocument>(std::move(answer.dump));
  m_hexRows = answer.rows;
  open(m_requested.value_or(desiredKey()));
}
