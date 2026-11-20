#pragma once

#include "formats/color_codes.h"
#include "formats/styled_text.h"
#include "imaging/text_document_loader.h"
#include "imaging/text_tile_renderer.h"

#include <string>
#include <utility>

// How a text tile view presents a document: colors, wrapping and words.
namespace luciole::text_tile_view {
  // Style roles in the palette's colors over SurfaceContainerLow, made opaque.
  TextTileTheme paletteTheme();

  // Prose, tables, rules and framed code follow the view's width; plain code sets its own.
  bool wraps(const formats::StyledDocument& document);

  // The words a loader's result is shown with, and whether they sit above its text; empty for none.
  std::pair<std::string, bool> wordsOf(const TextDocument& source);

  // "#rrggbb", or "#rrggbbaa" for a translucent color.
  std::string colorText(const formats::Rgba& color);
} // namespace luciole::text_tile_view
