#pragma once

#include "workbench/measure.h"
#include "workbench/tree.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace luciole::workbench {
  // Gamma-encoded sRGB, 8 bits per channel.
  struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    bool operator==(const Rgb&) const = default;
  };

  // Stored in Node::category. Other is 0, so a default node is uncategorized.
  enum class FileCategory : std::uint8_t {
    Other,
    Folder,
    Image,
    Video,
    Audio,
    Document,
    Code,
    Archive,
    Data,
    Executable
  };
  inline constexpr std::size_t kFileCategoryCount = 10;

  // Base accents, ink candidates and the roles the scenes draw states and chrome with, each named as its palette
  // role. Defaults are the default Luciole palette.
  struct ColorPalette {
    std::array<Rgb, kFileCategoryCount> categories{{
        {0x7D, 0x80, 0xA8}, // Other
        {0xB3, 0xB8, 0xED}, // Folder
        {0xA0, 0xDD, 0xC0}, // Image
        {0xED, 0x82, 0x96}, // Video
        {0xF2, 0xB7, 0x8C}, // Audio
        {0xF7, 0xE9, 0xA8}, // Document
        {0x8F, 0xC8, 0xF2}, // Code
        {0xD9, 0xA8, 0xE8}, // Archive
        {0x7F, 0xD6, 0xD0}, // Data
        {0xC9, 0xCC, 0xE6}, // Executable
    }};
    std::array<Rgb, 4> ramp{{{0x2E, 0x2F, 0x58}, {0x5B, 0x60, 0xA8}, {0xA0, 0xDD, 0xC0}, {0xF7, 0xE9, 0xA8}}};
    Rgb surface{0x15, 0x16, 0x2E};
    Rgb onSurface{0xEC, 0xE9, 0xF4};
    Rgb onSurfaceVariant{0xA5, 0xA7, 0xC6};
    Rgb surfaceContainerLow{0x1A, 0x1B, 0x35};
    Rgb surfaceContainer{0x20, 0x21, 0x3D};
    Rgb surfaceContainerHigh{0x27, 0x28, 0x4A};
    Rgb surfaceContainerHighest{0x2E, 0x2F, 0x58};
    Rgb outlineVariant{0x3E, 0x41, 0x66};
    Rgb primary{0xF7, 0xE9, 0xA8};   // the cursor and the way to the current folder
    Rgb secondary{0xB3, 0xB8, 0xED}; // merged pieces
    Rgb tertiary{0xA0, 0xDD, 0xC0};  // the piece another pane points at
    Rgb shadow{0x00, 0x00, 0x00};    // what raised cards cast
  };

  // Accent is the category or ramp color, fill a darker large-area version of it, ink the text color on fill.
  struct NodeColor {
    Rgb fill;
    Rgb ink;
    Rgb accent;
    bool operator==(const NodeColor&) const = default;
  };

  // Lightness, chroma and hue in radians.
  struct Oklch {
    double l = 0;
    double c = 0;
    double h = 0;
  };

  // Smallest and largest raw value among the visible entries of a folder, for one color measure.
  struct ColorRange {
    double min = 0;
    double max = 0;
  };

  // ASCII-lowercased text after the last dot; empty when that dot is the first or last character, or absent.
  std::string fileExtension(std::string_view name);
  FileCategory fileCategory(std::string_view name, bool folder);

  // Log-scale position of value between min and max, clamped to 0..1; 0.5 for an empty or invalid range.
  double normalize(double value, double min, double max);
  Rgb mix(Rgb from, Rgb to, double amount);
  // Position 0..1 on the four-stop ramp; out-of-range positions clamp and NaN reads as 0.
  Rgb ramp(double position, const std::array<Rgb, 4>& stops);
  // WCAG relative luminance and contrast ratio.
  double luminance(Rgb color);
  double contrast(Rgb first, Rgb second);
  Oklch toOklch(Rgb color);
  // Chroma is reduced until the color fits in sRGB.
  Rgb fromOklch(Oklch color);
  // Same hue, lightness 0.30 to 0.46 by the accent's own lightness, 95% of its chroma up to 0.15.
  Rgb fillFor(Rgb accent);
  // onSurface or surface, whichever contrasts more with fill; ties pick onSurface.
  Rgb inkFor(Rgb fill, const ColorPalette& palette);

  // Zero range for a folder without visible entries.
  ColorRange colorRange(const Tree& tree, NodeId folder, WorkbenchColor colorBy, bool showHidden);
  // Uses `range` for measure colors and reads no siblings, so a layout computes one range per folder.
  // An unknown id gets all-zero colors; an unknown category byte colors as Other.
  NodeColor
  nodeColor(const Tree& tree, NodeId id, WorkbenchColor colorBy, ColorRange range, const ColorPalette& palette);
  // Computes the range over the node's visible siblings; a node without a parent sits at the middle of the ramp.
  NodeColor nodeColor(const Tree& tree, NodeId id, const LayoutSettings& settings, const ColorPalette& palette = {});
} // namespace luciole::workbench
