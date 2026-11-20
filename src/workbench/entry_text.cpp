#include "workbench/entry_text.h"

#include "workbench/color.h"
#include "workbench/format.h"
#include "workbench/not_counted.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

namespace luciole::workbench {
  namespace {
    std::string typeOf(const std::string& name) {
      auto type = fileExtension(name);
      std::ranges::transform(type, type.begin(), [](char c) { return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c; });
      return type.empty() ? "file" : type;
    }

    bool tellsState(std::string_view text) { return text == kMeasuringLabel || text == kNotMeasuredLabel; }
  } // namespace

  EntryFacts entryFacts(const Tree& tree, NodeId id, const LayoutSettings* settings) {
    if (!hasNode(tree, id)) {
      return {};
    }
    const auto& node = tree.nodes[id];
    EntryFacts facts{
        .name = node.name.empty() ? std::string("/") : node.name,
        .size = measureLabel(tree, id, WorkbenchMeasure::Bytes),
        .items = node.folder ? measureLabel(tree, id, WorkbenchMeasure::Items) : std::string{},
        .files = node.folder ? measureLabel(tree, id, WorkbenchMeasure::Files) : std::string{},
        .type = node.folder ? std::string{} : typeOf(node.name),
        .modified = "modified " + agoText(age(tree, id)),
        .opens = measureValue(tree, id, WorkbenchMeasure::Visits) > 0 ? measureLabel(tree, id, WorkbenchMeasure::Visits)
                                                                      : std::string{},
        .share = {},
        .reasons = folderReasons(tree, id),
        .hidden = node.hidden,
        .notCounted = notCountedText(tree, id),
    };
    // Siblings of a folder above the open one are only listed, so a share among them says nothing.
    if (settings != nullptr && !std::ranges::contains(tree.ancestors, id)) {
      facts.share = approximateText(shareState(tree, id, *settings), shareText(share(tree, id, *settings)));
    }
    // The sentence names the folders on another filesystem and the unreadable ones, so their tags would repeat it.
    if (!facts.notCounted.empty()) {
      std::erase_if(facts.reasons, [](std::string_view reason) {
        return reason == kOtherFilesystemLabel || reason == kUnreadableLabel;
      });
    }
    return facts;
  }

  std::string entryText(const EntryFacts& facts) {
    if (facts.name.empty()) {
      return {};
    }
    std::string line = facts.size;
    // "measuring…" and "not measured" are told once.
    bool stateSaid = tellsState(line);
    const auto add = [&](std::string_view fact) {
      if (fact.empty() || (tellsState(fact) && std::exchange(stateSaid, true))) {
        return;
      }
      line += kFactsDot;
      line += fact;
    };
    add(facts.share);
    add(facts.items);
    add(facts.files);
    add(facts.type);
    add(facts.modified);
    add(facts.opens);
    for (const auto reason : facts.reasons) {
      add(reason);
    }
    if (facts.hidden) {
      add("hidden");
    }
    std::string text = facts.name + "\n" + line;
    if (!facts.notCounted.empty()) {
      text += "\n" + facts.notCounted;
    }
    return text;
  }

  std::string mergedText(const Tree& tree, std::span<const NodeId> nodes) {
    double bytes = 0;
    auto state = MeasureState::Done;
    std::size_t held = 0;
    for (const auto id : nodes) {
      if (hasNode(tree, id)) {
        ++held;
        bytes += measureValue(tree, id, WorkbenchMeasure::Bytes);
        state = worseState(state, measureState(tree.nodes[id]));
      }
    }
    if (state != MeasureState::LowerBound) {
      return {};
    }
    return std::format(
        "{} more entries\n{}\n{}", countText(static_cast<double>(held)), stateText(state, bytes, sizeText(bytes)),
        notCountedText(tree, nodes)
    );
  }
} // namespace luciole::workbench
