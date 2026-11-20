#include "workbench/iris_overlay.h"

#include "workbench/format.h"
#include "workbench/measure.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace luciole::workbench::iris {
  namespace {
    constexpr double kStackWidth = 300.0;
    constexpr double kBodyShare = 0.82; // the hub's lines and chips stay within this share of its diameter
    constexpr double kLineGap = 1.0;
    constexpr double kNoteGap = 4.0;
    constexpr double kChipsGap = 6.0;
    constexpr double kChipGap = 3.0;
    constexpr double kReadoutInset = 6.0;
    constexpr double kReadoutBottom = 4.0;
    constexpr double kReadoutLine = 18.0;
    constexpr double kPlainSize = 11.0; // the size TextMeasure::advance is given at
    constexpr double kWidthTolerance = 1e-6;
    constexpr double kMinNoteSize = 8.0; // the note shrinks no further to stay inside the hub
    constexpr double kNoteStep = 0.5;

    std::string displayName(const Node& node) { return node.name.empty() ? std::string("/") : node.name; }

    void
    addLine(Hub& hub, HubInk ink, std::string text, double size, double room, bool bold, const TextMeasure& measure) {
      const double width = std::min(measure.width(text, size, bold), room);
      hub.lines.push_back(
          {.ink = ink,
           .text = std::move(text),
           .box = {.x = hub.center.x - width / 2, .y = 0, .width = width, .height = size * kHubLineHeight},
           .fontSize = size}
      );
    }

    struct Headline {
      std::string text;
      double size = 0;
    };

    // The hub's size line: the label at `start`, shrunk toward `floor` to meet `room`. A lower bound whose full
    // wording does not fit even at the floor is written with the short prefix instead. None when neither fits whole.
    std::optional<Headline>
    headline(const Tree& tree, NodeId folder, double start, double floor, double room, const TextMeasure& measure) {
      const auto fitted = [&](std::string text) -> std::optional<Headline> {
        double size = start;
        if (const double natural = measure.width(text, size, true); natural > room) {
          size = std::max(floor, size * room / natural);
        }
        if (measure.width(text, size, true) > room + kWidthTolerance) {
          return std::nullopt;
        }
        return Headline{.text = std::move(text), .size = size};
      };
      if (auto whole = fitted(measureLabel(tree, folder, WorkbenchMeasure::Bytes))) {
        return whole;
      }
      return fitted(measureLabel(tree, folder, WorkbenchMeasure::Bytes, PrefixForm::Short));
    }

    void addChips(Hub& hub, const Tree& tree, std::span<const NodeId> trail, double room, const TextMeasure& measure) {
      for (auto chip : trailChips(trail.size())) {
        const auto id = trail[chip.index];
        if (!hasNode(tree, id)) {
          continue;
        }
        const auto name = measure.fit(displayName(tree.nodes[id]), hub.chipWidth, hub.chipFontSize);
        chip.node = id;
        chip.text = chip.gap ? std::format("… {}", name) : name;
        chip.fontSize = hub.chipFontSize;
        const double width = std::min(measure.width(chip.text, chip.fontSize) + 2 * kChipPad, room);
        chip.box = {.x = hub.center.x - width / 2, .y = 0, .width = width, .height = kChipHeight};
        hub.chips.push_back(std::move(chip));
      }
    }

    struct Wrapped {
      std::vector<std::string> lines;
      bool cut = false; // a word was cut to fit
    };

    // `text` broken at its spaces into lines no wider than `room`, each with as many words as fit; a word wider than
    // the room is cut in the middle to fit it.
    Wrapped wrapLines(std::string_view text, double room, double size, const TextMeasure& measure) {
      Wrapped wrapped;
      auto& lines = wrapped.lines;
      std::string line;
      // Each step takes one word and moves past it: at most text.size() + 1 steps.
      for (std::size_t start = 0; start <= text.size();) {
        const std::size_t end = std::min(text.find(' ', start), text.size());
        const auto word = text.substr(start, end - start);
        start = end + 1;
        if (word.empty()) {
          continue;
        }
        auto joined = line.empty() ? std::string(word) : std::format("{} {}", line, word);
        if (measure.width(joined, size) <= room) {
          line = std::move(joined);
          continue;
        }
        if (!line.empty()) {
          lines.push_back(std::move(line));
        }
        line = measure.fit(word, room, size);
        wrapped.cut = wrapped.cut || line != word;
      }
      if (!line.empty()) {
        lines.push_back(std::move(line));
      }
      return wrapped;
    }

    // Room above line `i`: the gap between lines, and more above the note's first line.
    double gapAbove(const Hub& hub, std::size_t i) {
      const bool noteStarts = hub.lines[i].ink == HubInk::Note && (i == 0 || hub.lines[i - 1].ink != HubInk::Note);
      return (i > 0 ? kLineGap : 0) + (noteStarts ? kNoteGap : 0);
    }

    // Stacks the lines, then the chips, so the whole body is centered on the hub.
    void stackBody(Hub& hub) {
      double height = 0;
      for (std::size_t i = 0; i < hub.lines.size(); ++i) {
        height += gapAbove(hub, i) + hub.lines[i].box.height;
      }
      if (!hub.chips.empty()) {
        const auto count = static_cast<double>(hub.chips.size());
        height += (hub.lines.empty() ? 0 : kLineGap) + kChipsGap + count * kChipHeight + (count - 1) * kChipGap;
      }
      double y = hub.center.y - height / 2;
      for (std::size_t i = 0; i < hub.lines.size(); ++i) {
        y += gapAbove(hub, i);
        hub.lines[i].box.y = y;
        y += hub.lines[i].box.height;
      }
      y += (hub.lines.empty() ? 0 : kLineGap) + kChipsGap;
      for (auto& chip : hub.chips) {
        chip.box.y = y;
        y += kChipHeight + kChipGap;
      }
    }

    // Every corner of every line of the note lies inside the hub's disc.
    bool noteInside(const Hub& hub) {
      return std::ranges::all_of(hub.lines, [&hub](const HubLine& line) {
        const double top = std::abs(line.box.y - hub.center.y);
        const double bottom = std::abs(line.box.y + line.box.height - hub.center.y);
        return line.ink != HubInk::Note
            || std::hypot(line.box.width / 2, std::max(top, bottom)) <= hub.radius - kNoteClearance;
      });
    }

    // "⋯ N more levels · name" with the name cut to what is left of `room`; without it when none is, and empty when
    // `room` cannot hold the count and its words, which are never cut.
    std::string
    foldedText(std::size_t levels, std::string_view name, double room, double size, const TextMeasure& measure) {
      const auto count = std::format("⋯ {} more levels", levels);
      if (measure.width(count, size) > room) {
        return {};
      }
      const std::string joined = count + " · ";
      const auto fitted = measure.fit(name, room - measure.width(joined, size), size);
      return fitted.empty() ? count : joined + fitted;
    }

    // `text` ended with an ellipsis, cut at a code point so that it is no wider than `room`.
    std::string withEllipsis(std::string text, double room, double size, const TextMeasure& measure) {
      constexpr std::string_view kEllipsis = "…";
      // Each step removes one code point of the text.
      while (!text.empty() && measure.width(text + std::string(kEllipsis), size) > room) {
        auto last = text.size() - 1;
        while (last > 0 && (static_cast<unsigned char>(text[last]) & 0xC0U) == 0x80U) {
          --last;
        }
        text.erase(last);
      }
      return text + std::string(kEllipsis);
    }

    // Ends a line with an ellipsis, as wide as `room` at most.
    void endWithEllipsis(Hub& hub, HubLine& line, double room, const TextMeasure& measure) {
      line.text = withEllipsis(std::move(line.text), room, line.fontSize, measure);
      line.box.width = std::min(measure.width(line.text, line.fontSize), room);
      line.box.x = hub.center.x - line.box.width / 2;
    }

    // Makes room for the note inside the hub: first the count line and then the size line, which a small pane's
    // readout says again, then the note's lines from the end, the last one left ending in an ellipsis. Each pass
    // removes a line.
    void trimNote(Hub& hub, std::size_t kept, double room, const TextMeasure& measure) {
      std::size_t noteFrom = kept;
      for (;;) {
        stackBody(hub);
        if (hub.lines.size() == noteFrom || noteInside(hub)) {
          return;
        }
        const auto facts = [&hub](HubInk ink) { return std::ranges::find(hub.lines, ink, &HubLine::ink); };
        auto gone = facts(HubInk::Items);
        if (gone == hub.lines.end()) {
          gone = facts(HubInk::Total);
        }
        if (gone != hub.lines.end()) {
          hub.lines.erase(gone);
          --noteFrom;
          continue;
        }
        hub.lines.pop_back();
        if (hub.lines.size() > noteFrom) {
          endWithEllipsis(hub, hub.lines.back(), room, measure);
        }
      }
    }

    // Adds the note's lines and stacks the body: at `size` when the wrapped note cuts no word and stays inside the
    // hub, else at the largest size down to kMinNoteSize, in kNoteStep steps, at which it does. When none does, the
    // note takes the smallest size and loses the lines that would reach the border.
    void addNote(Hub& hub, double room, double size, const TextMeasure& measure) {
      const std::size_t kept = hub.lines.size();
      const double smallest = std::min(size, kMinNoteSize);
      const double span = size - smallest;
      const auto steps = std::isfinite(span) && span > 0 ? static_cast<int>(std::ceil(span / kNoteStep)) : 0;
      for (int step = 0; step <= steps; ++step) {
        const double at = std::max(smallest, size - (step * kNoteStep));
        hub.lines.resize(kept);
        auto wrapped = wrapLines(hub.note, room, at, measure);
        for (auto& line : wrapped.lines) {
          addLine(hub, HubInk::Note, std::move(line), at, room, false, measure);
        }
        stackBody(hub);
        if (!wrapped.cut && noteInside(hub)) {
          return;
        }
        if (step == steps) {
          trimNote(hub, kept, room, measure);
        }
      }
    }
  } // namespace

  double TextMeasure::width(std::string_view text, double size, bool bold) const {
    if (!(size > 0) || !std::isfinite(size)) {
      return 0;
    }
    if (bold && boldFont) {
      return labelTextWidth(text, *boldFont) * size / kBaseSize;
    }
    const double stretch = bold ? kHubSemiboldWidth : 1.0;
    if (font) {
      return labelTextWidth(text, *font) * size / kBaseSize * stretch;
    }
    return static_cast<double>(codePointCount(text)) * advance * size / kPlainSize * stretch;
  }

  std::string TextMeasure::fit(std::string_view text, double room, double size, bool bold) const {
    if (text.empty() || !(room > 0) || !(size > 0) || !std::isfinite(size)) {
      return {};
    }
    if (width(text, size, bold) <= room) {
      return std::string(text);
    }
    // Binary search over the code points kept: log2 of the length steps.
    std::size_t low = 1;
    std::size_t high = codePointCount(text) - 1;
    while (low < high) {
      const std::size_t middle = (low + high + 1) / 2;
      if (width(middleEllipsis(text, middle), size, bold) <= room) {
        low = middle;
      } else {
        high = middle - 1;
      }
    }
    return middleEllipsis(text, std::max<std::size_t>(1, low));
  }

  std::vector<TrailChip> trailChips(std::size_t trailLength) {
    const auto chip = [](std::size_t index, bool gap) {
      return TrailChip{.index = index, .gap = gap, .node = kNoNode, .text = {}, .box = {}, .fontSize = 0};
    };
    if (trailLength > 3) {
      return {chip(0, false), chip(1, false), chip(trailLength - 1, true)};
    }
    std::vector<TrailChip> chips;
    chips.reserve(trailLength);
    for (std::size_t i = 0; i < trailLength; ++i) {
      chips.push_back(chip(i, false));
    }
    return chips;
  }

  Hub hubLayout(
      const Tree& tree, const ring::Dial& dial, NodeId folder, std::span<const NodeId> trail, std::string note,
      const TextMeasure& measure
  ) {
    const bool compact = dial.compact;
    const double scale = compact ? 1.0 : std::max(1.0, std::min(1.9, dial.rHub / 130.0));
    const double inner = std::floor(dial.rHub * (compact ? 1.7 : 1.5));
    Hub hub;
    hub.center = dial.center;
    hub.radius = dial.rHub;
    hub.fontSize = compact ? 12.0 : 13.0 * scale;
    hub.nameWidth = inner;
    hub.chipWidth = inner - 26.0;
    hub.chipFontSize = 11.0 * scale;
    hub.tiny = compact && dial.rHub < 26.0;
    hub.showItems = !compact || dial.rHub >= 40.0;
    hub.showTrail = !compact || dial.rHub >= 52.0;
    hub.canGoUp = !tree.ancestors.empty();
    hub.note = std::move(note);
    if (!hasNode(tree, folder)) {
      return hub;
    }
    const auto& node = tree.nodes[folder];
    hub.name = displayName(node);
    hub.total = measureLabel(tree, folder, WorkbenchMeasure::Bytes);
    hub.items = itemsLabel(tree, folder);
    if (hub.tiny) {
      return hub;
    }
    const double body = 2 * hub.radius * kBodyShare;
    const double size = hub.fontSize;
    // The name is cut to its width; the size and the count may span the whole disc, the size shrinking toward
    // the name's size.
    const double disc = 2 * hub.radius;
    const double itemsSize = compact ? 11.0 : size * 0.85;
    const double totalSize = size * (compact ? 1.25 : 1.45);
    // The name is semibold: cut so that it is as wide as the room when drawn.
    addLine(hub, HubInk::Name, measure.fit(hub.name, hub.nameWidth, size, true), size, body, true, measure);
    // A shrunk size meets the disc exactly; the tolerance keeps rounding from cutting it. A size and a count are
    // shown whole, or left out when the hub has no room for them.
    const double totalRoom = disc + kWidthTolerance;
    if (auto total = headline(tree, folder, totalSize, size, disc, measure)) {
      addLine(hub, HubInk::Total, std::move(total->text), total->size, totalRoom, true, measure);
    }
    if (hub.showItems && measure.width(hub.items, itemsSize) <= totalRoom) {
      addLine(hub, HubInk::Items, hub.items, itemsSize, disc, false, measure);
    }
    if (hub.showTrail) {
      addChips(hub, tree, trail, body, measure);
    }
    addNote(hub, body, size * 0.85, measure);
    return hub;
  }

  AncestorStack ancestorStack(const Tree& tree, const ring::Dial& dial, const TextMeasure& measure) {
    AncestorStack stack;
    const auto slots = dial.stackRows;
    if (slots == 0) {
      return stack;
    }
    const auto& ancestors = tree.ancestors;
    const auto shown = std::min(ancestors.size(), slots - 1);
    const double width = std::min(kStackWidth, dial.width - 8.0);
    stack.box = {
        .x = std::max(4.0, dial.center.x - width / 2),
        .y = dial.stackBottom - dial.stackRow * static_cast<double>(slots),
        .width = width,
        .height = dial.stackRow * static_cast<double>(slots)
    };
    stack.fontSize = dial.compact ? 11.0 : 12.0;
    stack.textWidth = width - 20.0;
    const auto nameOf = [&tree](NodeId id) {
      return hasNode(tree, id) ? displayName(tree.nodes[id]) : std::string("/");
    };
    const bool folds = ancestors.size() > shown;
    if (folds) {
      const auto top = ancestors.back();
      stack.rows.push_back({.node = top, .levels = ancestors.size(), .text = nameOf(top), .opacity = 0.5, .box = {}});
    }
    for (std::size_t k = shown; k-- > 0;) {
      stack.rows.push_back(
          {.node = ancestors[k],
           .levels = k + 1,
           .text = nameOf(ancestors[k]),
           .opacity = std::max(0.5, 1.0 - static_cast<double>(k) * 0.12),
           .box = {}}
      );
    }
    const double middle = stack.box.x + stack.box.width / 2;
    const auto count = static_cast<double>(stack.rows.size());
    for (std::size_t i = 0; i < stack.rows.size(); ++i) {
      auto& row = stack.rows[i];
      row.text = folds && i == 0
          ? foldedText(ancestors.size() - shown, row.text, stack.textWidth, stack.fontSize, measure)
          : measure.fit(row.text, stack.textWidth, stack.fontSize);
      const double rowWidth = measure.width(row.text, stack.fontSize) + 2 * kStackPad;
      row.box = {
          .x = middle - rowWidth / 2,
          .y = dial.stackBottom - (count - static_cast<double>(i)) * dial.stackRow,
          .width = rowWidth,
          .height = dial.stackRow
      };
    }
    return stack;
  }

  std::optional<Readout> readout(const ring::Dial& dial) {
    if (!dial.compact) {
      return std::nullopt;
    }
    return Readout{
        .box =
            {.x = kReadoutInset,
             .y = dial.height - kReadoutBottom - kReadoutLine,
             .width = std::max(0.0, dial.width - 2 * kReadoutInset),
             .height = kReadoutLine},
        .fontSize = kReadoutFont,
    };
  }
} // namespace luciole::workbench::iris
