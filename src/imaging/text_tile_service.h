#pragma once

#include "formats/styled_text.h"
#include "imaging/text_tile_renderer.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace luciole {
  inline constexpr std::size_t kTextTileRequestLimit = 64; // tiles one request may ask for
  inline constexpr std::size_t kTextTileOutboxLimit = 4;   // finished results waiting for the main context

  // A layout's size and where its lines start, in device pixels.
  struct TextTileGeometry {
    int width = 0;
    int height = 0;
    std::size_t tileCount = 0;
    bool truncated = false;
    std::vector<int> lineTops; // TextTileLayout::lineTops
    bool operator==(const TextTileGeometry&) const = default;
  };

  // Runs on the default main context, only for the latest revision of a document that is still open.
  struct TextTileEvents {
    std::function<void(std::uint64_t revision, std::expected<TextTileGeometry, std::string> geometry)> laidOut;
    std::function<void(std::uint64_t revision, std::size_t index, std::expected<TextTile, std::string> tile)> tile;
  };

  namespace detail {
    struct TextTileServiceState;
  }

  // One open document of a TextTileService. Closing it, or dropping the handle, cancels its queued work, stops its
  // events and frees its layout on the worker; a tile being drawn finishes and is dropped.
  class [[nodiscard]] TextTileDocument {
  public:
    TextTileDocument() = default;
    ~TextTileDocument();
    TextTileDocument(TextTileDocument&& other) noexcept;
    TextTileDocument& operator=(TextTileDocument&& other) noexcept;
    TextTileDocument(const TextTileDocument&) = delete;
    TextTileDocument& operator=(const TextTileDocument&) = delete;

    // Open, on a service that still exists.
    bool valid() const;
    // The latest revision; 0 for an invalid handle.
    std::uint64_t revision() const;
    // Lays the document out again as the next revision and drops queued tiles; returns that revision, or 0 for an
    // invalid handle.
    std::uint64_t relayout(const TextTileOptions& options);
    // Replaces the queued tiles with the first kTextTileRequestLimit of `indices`, drawn in that order from the
    // latest revision. An index past the layout's tiles gives an error.
    void request(std::span<const std::size_t> indices);
    void close();

  private:
    friend class TextTileService;
    TextTileDocument(std::weak_ptr<detail::TextTileServiceState> service, std::uint64_t id);

    std::weak_ptr<detail::TextTileServiceState> m_service;
    std::uint64_t m_id = 0;
  };

  // Lays out styled documents and draws their tiles on one worker thread of its own, which owns every layout.
  // Requests and events belong to the default main context. The worker sleeps while no work is queued, and it stops
  // drawing while kTextTileOutboxLimit results wait for the main context. Each document queues at most one layout and
  // kTextTileRequestLimit tiles; the most recently asked document is served first.
  class TextTileService {
  public:
    TextTileService();
    // Cancels queued work and waits for the worker.
    ~TextTileService();
    TextTileService(const TextTileService&) = delete;
    TextTileService& operator=(const TextTileService&) = delete;

    // Opens a document as revision 1 and queues its layout.
    TextTileDocument open(
        std::shared_ptr<const formats::StyledDocument> document, const TextTileOptions& options, TextTileEvents events
    );

  private:
    std::shared_ptr<detail::TextTileServiceState> m_state;
  };
} // namespace luciole
