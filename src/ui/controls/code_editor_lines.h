#pragma once

#include "imaging/text_line_strip.h"
#include "render/core/texture_handle.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

class ImageNode;
class Node;
class TextureManager;

// The drawn lines of a code editor: one texture per line, kept until the line's content or style changes.
namespace luciole::code_editor {
  // Device pixels of a line a strip holds: [x0, x0 + width).
  struct StripWindow {
    int x0 = 0;
    int width = 0;
    bool operator==(const StripWindow&) const = default;
  };

  // The window a strip of a line `lineWidth` device pixels wide draws so that the viewport's device pixels
  // [left, left + width) of the line are covered: the whole line when it fits one texture, else the widest texture
  // centered on the viewport and kept inside the line. Empty for a line without width.
  StripWindow stripWindow(int lineWidth, int left, int width);

  // Code points of valid UTF-8 text: the columns it takes in a styled line.
  std::size_t codePoints(std::string_view text);

  // The line with `text` inserted at `column`, or at its end past it, as a plain run; swatches at or after the column
  // move past the text.
  formats::StyledLine withText(const formats::StyledLine& line, std::size_t column, std::string_view text);

  // Transparent device pixels around a strip's texture: an image node softens its outermost pixels, so they hold no
  // ink.
  inline constexpr int kStripBorder = 1;

  // The strip's pixels in the straight alpha image nodes draw, framed by kStripBorder transparent pixels each side;
  // empty for a strip without pixels or whose size does not match them.
  std::vector<std::uint8_t> imageOf(const TextStrip& strip);

  // Strips of lines by index, each drawn by an image node of `layer`, which it owns. Destroying it, or clear(),
  // unloads every texture.
  class LineStrips {
  public:
    explicit LineStrips(Node& layer);
    ~LineStrips();
    LineStrips(const LineStrips&) = delete;
    LineStrips& operator=(const LineStrips&) = delete;

    std::size_t count() const { return m_lines.size(); }
    std::size_t bytes() const;
    // The line holds a strip that is not stale and covers the viewport's device pixels [left, left + width), or, for a
    // viewport wider than one texture, the widest texture centered on it.
    bool current(std::size_t line, int left, int width) const;

    // Uploads the strip of a line `lineWidth` device pixels wide as imageOf() frames it, replacing the line's old
    // one. A strip without pixels marks the line drawn and empty.
    void put(TextureManager& textures, std::size_t line, const TextStrip& strip, int lineWidth);
    // Lines [first, first + count) keep their pixels on screen until they are drawn again.
    void markStale(std::size_t first, std::size_t count);
    // Lines [first, first + removed) were replaced by `inserted` lines: the first of them keep their old pixels as
    // stale, the rest go, and the lines after move without being drawn again.
    void edited(std::size_t first, std::size_t removed, std::size_t inserted);
    // Unloads every line outside [first, last].
    void keepOnly(std::size_t first, std::size_t last);
    void clear();
    // Drops the strips without unloading them, after the GPU lost every texture.
    void forget();

    // Places line i's node at device pixels (originX + x0, originY + i * lineHeight), on whole device pixels of
    // `scale`.
    void place(double scale, int originX, int originY, int lineHeight);

  private:
    struct Resident {
      TextureHandle texture;
      ImageNode* node = nullptr; // none for an empty line
      StripWindow window;
      int height = 0;
      int lineWidth = 0;
      bool stale = false;
    };

    void remove(std::map<std::size_t, Resident>::iterator it, bool unload);

    Node& m_layer;
    TextureManager* m_textures = nullptr;
    std::map<std::size_t, Resident> m_lines;
  };
} // namespace luciole::code_editor
