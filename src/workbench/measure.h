#pragma once

#include "model/directory_options.h"
#include "workbench/tree.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace luciole::workbench {
  struct LayoutSettings {
    WorkbenchMeasure sizeBy = WorkbenchMeasure::Bytes;
    WorkbenchColor colorBy = WorkbenchColor::Type;
    double emphasis = 0.5; // exponent applied to every weight, clamped to 0..1; 0 makes all entries equal
    bool showHidden = false;
    bool operator==(const LayoutSettings&) const = default;
  };

  // Visible children of a folder, heaviest first, with their weights and the sum of those weights.
  struct Entries {
    std::vector<NodeId> ids;
    std::vector<double> weights; // aligned with ids
    double total = 0;
  };

  struct EntrySplit {
    std::vector<NodeId> shown;
    std::vector<NodeId> rest;
  };

  // Raw value of a node: bytes, files, items, modified seconds, visits, or 1 for Equal. Non-finite or negative
  // values read as 0 and values above 1e30 as 1e30; an unknown id reads as 0.
  double measureValue(const Tree& tree, NodeId id, WorkbenchMeasure measure);

  // Seconds since the newest change below the node, never negative.
  double age(const Tree& tree, NodeId id);

  // (base + floor) ^ emphasis. Base is the raw value, or 1 / (1 + age / 30 days) for Modified; floors keep
  // empty entries visible. An unknown id weighs 0.
  double weight(const Tree& tree, NodeId id, const LayoutSettings& settings);

  // Orders by weight, then compareNames, then name bytes, then id.
  Entries entries(const Tree& tree, NodeId folder, const LayoutSettings& settings);

  EntrySplit splitEntries(const Tree& tree, NodeId folder, const LayoutSettings& settings, std::size_t maxShown);

  // Weight over the total of the visible siblings: 1 without a parent, 0 for a hidden entry or an unknown id.
  double share(const Tree& tree, NodeId id, const LayoutSettings& settings);

  // share() of every child of `folder`, aligned with children(tree, folder), from one sort of the weights. Empty
  // for an unknown id or a node without children.
  std::vector<double> shares(const Tree& tree, NodeId folder, const LayoutSettings& settings);

  // How far a size or count can be trusted, in order of doubt: finished, a lower bound of a walk that was cut short
  // and is not running, or still being measured.
  enum class MeasureState : std::uint8_t { Done, LowerBound, Measuring };

  // Separates the facts in a label or a tooltip.
  inline constexpr std::string_view kFactsDot = "  ·  ";

  // What a label says of a value of zero that is not final: still being measured, or never measured.
  inline constexpr std::string_view kMeasuringLabel = "measuring…";
  inline constexpr std::string_view kNotMeasuredLabel = "not measured";
  // Goes before a lower bound above zero.
  inline constexpr std::string_view kAtLeastPrefix = "at least ";
  // Stands for kAtLeastPrefix where a label is too tight for it.
  inline constexpr std::string_view kAtLeastShortPrefix = "≥ ";
  enum class PrefixForm : std::uint8_t { Long, Short };
  // Goes before a share that is not final.
  inline constexpr std::string_view kAboutPrefix = "about ";

  // Why a folder's entries are not all known, in the words every tooltip uses.
  inline constexpr std::string_view kOtherFilesystemLabel = "on another filesystem";
  inline constexpr std::string_view kNotExpandedLabel = "not expanded";
  inline constexpr std::string_view kUnreadableLabel = "unreadable";

  // Measuring when the node is partial, else a lower bound when it is one, else done.
  MeasureState measureState(const Node& node);

  // The more doubtful state of two: measuring, then a lower bound, then done. For sums of folders.
  MeasureState worseState(MeasureState left, MeasureState right);

  // How far a node's share can be trusted: the worst state of the node and of every sibling in the share's total.
  // Done for the root, a hidden entry that is not shown and an unknown id.
  MeasureState shareState(const Tree& tree, NodeId id, const LayoutSettings& settings);

  // `text` with "about " before it unless the state is done; empty text stays empty.
  std::string approximateText(MeasureState state, std::string_view text);

  // `text` as written for a value: unchanged when done or measuring with a value above zero, kMeasuringLabel for a
  // zero still being measured, kNotMeasuredLabel for a zero lower bound, "at least " before `text` for a lower bound
  // above zero; the short form writes kAtLeastShortPrefix there.
  std::string stateText(MeasureState state, double value, std::string_view text, PrefixForm form = PrefixForm::Long);

  // The reasons a folder's entries or size are not known: another filesystem, not expanded, unreadable, in that
  // order, for the flags the node has. "Not expanded" is left out when the size already reads measuring or not
  // measured, which tells it. Empty for a file and a listed folder; none for an unknown id.
  std::vector<std::string_view> folderReasons(const Tree& tree, NodeId id);

  // "600 B", "5 opens", "2 items", "3 files", "33 min ago"; an en dash for item or file counts of a file. A folder's
  // size, item count or file count goes through stateText with the state of its node, and `form` is passed on.
  std::string measureLabel(const Tree& tree, NodeId id, WorkbenchMeasure measure, PrefixForm form = PrefixForm::Long);

  // "2,000 items" for a folder, as measureLabel writes it for Items.
  std::string itemsLabel(const Tree& tree, NodeId id);

  // Compares digit runs by value, ASCII letters without case, and other characters in the order: controls,
  // space and punctuation, digits, letters, then non-ASCII by code point. Equal names may differ in bytes.
  std::weak_ordering compareNames(std::string_view left, std::string_view right);
} // namespace luciole::workbench
