#include "workbench/measure.h"

#include "workbench/format.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace luciole::workbench {
  namespace {
    constexpr double kMaxValue = 1e30;
    constexpr double kRecentSpan = 30.0 * 86400.0;
    // Space and punctuation in sort order; they come after control characters and before digits.
    constexpr std::string_view kPunctuationOrder = " _-,;:!?.'\"()[]{}@*/\\&#%`^+<=>|~$";
    constexpr int kDigitRank = 40;
    constexpr int kLetterRank = 50;
    constexpr int kNonAsciiRank = 100;

    double clean(double value) { return std::isfinite(value) && value > 0 ? std::min(value, kMaxValue) : 0.0; }

    double floorFor(WorkbenchMeasure measure) {
      switch (measure) {
      case WorkbenchMeasure::Bytes:
      case WorkbenchMeasure::Files:
      case WorkbenchMeasure::Items:
      case WorkbenchMeasure::Visits:
        return 1.0;
      case WorkbenchMeasure::Modified:
        return 0.001;
      case WorkbenchMeasure::Equal:
        return 0.0;
      }
      return 0.0;
    }

    double clampedEmphasis(double emphasis) { return std::isnan(emphasis) ? 0.0 : std::clamp(emphasis, 0.0, 1.0); }

    double weightOf(const Tree& tree, NodeId id, WorkbenchMeasure measure, double emphasis) {
      const double base = measure == WorkbenchMeasure::Modified ? 1.0 / (1.0 + age(tree, id) / kRecentSpan)
                                                                : measureValue(tree, id, measure);
      return std::pow(base + floorFor(measure), emphasis);
    }

    bool isDigit(char c) { return c >= '0' && c <= '9'; }

    int rank(char c) {
      const auto byte = static_cast<unsigned char>(c);
      if (byte >= 0x80U) {
        return kNonAsciiRank + byte;
      }
      if (byte >= 'A' && byte <= 'Z') {
        return kLetterRank + (byte - 'A');
      }
      if (byte >= 'a' && byte <= 'z') {
        return kLetterRank + (byte - 'a');
      }
      if (isDigit(c)) {
        return kDigitRank;
      }
      const auto found = kPunctuationOrder.find(c);
      return found == std::string_view::npos ? 0 : 1 + static_cast<int>(found);
    }

    // Compares the digit runs starting at left[i] and right[j] by value and moves both indexes past them.
    std::weak_ordering compareDigitRuns(std::string_view left, std::size_t& i, std::string_view right, std::size_t& j) {
      // Significant digits of the run at `from`, without leading zeros; `from` moves past the run.
      const auto digits = [](std::string_view text, std::size_t& from) {
        auto end = from;
        while (end < text.size() && isDigit(text[end])) {
          ++end;
        }
        auto start = from;
        while (start + 1 < end && text[start] == '0') {
          ++start;
        }
        from = end;
        return text.substr(start, end - start);
      };
      const auto leftDigits = digits(left, i);
      const auto rightDigits = digits(right, j);
      if (leftDigits.size() != rightDigits.size()) {
        return leftDigits.size() <=> rightDigits.size();
      }
      return leftDigits.compare(rightDigits) <=> 0;
    }

    // Visible children's weights summed heaviest first, as entries() sums them, so shares match its order exactly.
    double siblingTotal(std::vector<double> weights) {
      std::ranges::sort(weights, std::ranges::greater{});
      double total = 0;
      for (const double value : weights) {
        total += value;
      }
      return total;
    }

    // Weight of each child, or a negative value for one that is hidden or unknown.
    std::vector<double> childWeights(const Tree& tree, NodeId folder, const LayoutSettings& settings) {
      const auto all = children(tree, folder);
      const double emphasis = clampedEmphasis(settings.emphasis);
      std::vector<double> weights;
      weights.reserve(all.size());
      for (const auto id : all) {
        const bool shown = hasNode(tree, id) && visibleEntry(tree.nodes[id], settings.showHidden);
        weights.push_back(shown ? weightOf(tree, id, settings.sizeBy, emphasis) : -1.0);
      }
      return weights;
    }

    double visibleTotal(const std::vector<double>& weights) {
      std::vector<double> visible;
      visible.reserve(weights.size());
      std::ranges::copy_if(weights, std::back_inserter(visible), [](double value) { return value >= 0; });
      return siblingTotal(std::move(visible));
    }

    std::string countLabel(double value, std::string_view one, std::string_view many) {
      const auto text = countText(value);
      return text + ' ' + std::string(text == "1" ? one : many);
    }
  } // namespace

  double measureValue(const Tree& tree, NodeId id, WorkbenchMeasure measure) {
    if (!hasNode(tree, id)) {
      return 0.0;
    }
    const auto& node = tree.nodes[id];
    switch (measure) {
    case WorkbenchMeasure::Bytes:
      return clean(node.bytes);
    case WorkbenchMeasure::Files:
      return clean(node.files);
    case WorkbenchMeasure::Items:
      return clean(node.items);
    case WorkbenchMeasure::Modified:
      return clean(node.modified);
    case WorkbenchMeasure::Visits:
      return clean(node.visits);
    case WorkbenchMeasure::Equal:
      return 1.0;
    }
    return 0.0;
  }

  double age(const Tree& tree, NodeId id) {
    if (!hasNode(tree, id)) {
      return 0.0;
    }
    return clean(tree.now - measureValue(tree, id, WorkbenchMeasure::Modified));
  }

  double weight(const Tree& tree, NodeId id, const LayoutSettings& settings) {
    if (!hasNode(tree, id)) {
      return 0.0;
    }
    return weightOf(tree, id, settings.sizeBy, clampedEmphasis(settings.emphasis));
  }

  Entries entries(const Tree& tree, NodeId folder, const LayoutSettings& settings) {
    Entries result;
    const auto all = children(tree, folder);
    const double emphasis = clampedEmphasis(settings.emphasis);
    std::vector<std::pair<double, NodeId>> weighted;
    weighted.reserve(all.size());
    for (const auto id : all) {
      if (hasNode(tree, id) && visibleEntry(tree.nodes[id], settings.showHidden)) {
        weighted.emplace_back(weightOf(tree, id, settings.sizeBy, emphasis), id);
      }
    }
    std::ranges::sort(weighted, [&tree](const auto& left, const auto& right) {
      if (left.first != right.first) {
        return left.first > right.first;
      }
      const auto& leftName = tree.nodes[left.second].name;
      const auto& rightName = tree.nodes[right.second].name;
      if (const auto order = compareNames(leftName, rightName); order != 0) {
        return order < 0;
      }
      if (leftName != rightName) {
        return leftName < rightName;
      }
      return left.second < right.second;
    });
    result.ids.reserve(weighted.size());
    result.weights.reserve(weighted.size());
    for (const auto& [entryWeight, id] : weighted) {
      result.ids.push_back(id);
      result.weights.push_back(entryWeight);
      result.total += entryWeight;
    }
    return result;
  }

  EntrySplit splitEntries(const Tree& tree, NodeId folder, const LayoutSettings& settings, std::size_t maxShown) {
    auto ids = entries(tree, folder, settings).ids;
    const auto cut = std::min(maxShown, ids.size());
    EntrySplit split;
    split.rest.assign(ids.begin() + static_cast<std::ptrdiff_t>(cut), ids.end());
    ids.resize(cut);
    split.shown = std::move(ids);
    return split;
  }

  double share(const Tree& tree, NodeId id, const LayoutSettings& settings) {
    if (!hasNode(tree, id)) {
      return 0.0;
    }
    const auto& node = tree.nodes[id];
    if (!hasNode(tree, node.parent)) {
      return 1.0;
    }
    if (!visibleEntry(node, settings.showHidden)) {
      return 0.0;
    }
    const double total = visibleTotal(childWeights(tree, node.parent, settings));
    return total > 0.0 ? weightOf(tree, id, settings.sizeBy, clampedEmphasis(settings.emphasis)) / total : 0.0;
  }

  std::vector<double> shares(const Tree& tree, NodeId folder, const LayoutSettings& settings) {
    auto weights = childWeights(tree, folder, settings);
    const double total = visibleTotal(weights);
    for (double& value : weights) {
      value = value >= 0 && total > 0.0 ? value / total : 0.0;
    }
    return weights;
  }

  MeasureState measureState(const Node& node) {
    return node.partial ? MeasureState::Measuring : node.lowerBound ? MeasureState::LowerBound : MeasureState::Done;
  }

  MeasureState worseState(MeasureState left, MeasureState right) {
    return std::to_underlying(left) >= std::to_underlying(right) ? left : right;
  }

  MeasureState shareState(const Tree& tree, NodeId id, const LayoutSettings& settings) {
    if (!hasNode(tree, id)
        || !hasNode(tree, tree.nodes[id].parent)
        || !visibleEntry(tree.nodes[id], settings.showHidden)) {
      return MeasureState::Done;
    }
    auto state = MeasureState::Done;
    for (const auto sibling : children(tree, tree.nodes[id].parent)) {
      if (hasNode(tree, sibling) && visibleEntry(tree.nodes[sibling], settings.showHidden)) {
        state = worseState(state, measureState(tree.nodes[sibling]));
      }
    }
    return state;
  }

  std::string approximateText(MeasureState state, std::string_view text) {
    return state == MeasureState::Done || text.empty() ? std::string(text)
                                                       : std::string(kAboutPrefix) + std::string(text);
  }

  std::string stateText(MeasureState state, double value, std::string_view text, PrefixForm form) {
    switch (state) {
    case MeasureState::Done:
      break;
    case MeasureState::Measuring:
      if (value == 0.0) {
        return std::string(kMeasuringLabel);
      }
      break;
    case MeasureState::LowerBound:
      if (value == 0.0) {
        return std::string(kNotMeasuredLabel);
      }
      return std::string(form == PrefixForm::Short ? kAtLeastShortPrefix : kAtLeastPrefix) + std::string(text);
    }
    return std::string(text);
  }

  std::vector<std::string_view> folderReasons(const Tree& tree, NodeId id) {
    if (!hasNode(tree, id)) {
      return {};
    }
    const auto& node = tree.nodes[id];
    const auto size = measureLabel(tree, id, WorkbenchMeasure::Bytes);
    const bool sizeTold = size == kMeasuringLabel || size == kNotMeasuredLabel;
    std::vector<std::string_view> reasons;
    if (node.otherFilesystem) {
      reasons.push_back(kOtherFilesystemLabel);
    }
    if (node.truncated && !sizeTold) {
      reasons.push_back(kNotExpandedLabel);
    }
    if (node.unreadable) {
      reasons.push_back(kUnreadableLabel);
    }
    return reasons;
  }

  std::string measureLabel(const Tree& tree, NodeId id, WorkbenchMeasure measure, PrefixForm form) {
    if (!hasNode(tree, id)) {
      return {};
    }
    const auto& node = tree.nodes[id];
    const bool folder = node.folder;
    // A folder's size or count as stateText writes it; a file's is always final.
    const auto counted = [&](WorkbenchMeasure by, const std::string& text) {
      const auto state = folder ? measureState(node) : MeasureState::Done;
      return stateText(state, measureValue(tree, id, by), text, form);
    };
    switch (measure) {
    case WorkbenchMeasure::Visits:
      return countLabel(measureValue(tree, id, measure), "open", "opens");
    case WorkbenchMeasure::Items:
      return folder ? counted(measure, countLabel(measureValue(tree, id, measure), "item", "items")) : "–";
    case WorkbenchMeasure::Files:
      return folder ? counted(measure, countLabel(measureValue(tree, id, measure), "file", "files")) : "–";
    case WorkbenchMeasure::Modified:
      return agoText(age(tree, id));
    case WorkbenchMeasure::Bytes:
    case WorkbenchMeasure::Equal:
      return counted(WorkbenchMeasure::Bytes, sizeText(measureValue(tree, id, WorkbenchMeasure::Bytes)));
    }
    return {};
  }

  std::string itemsLabel(const Tree& tree, NodeId id) {
    if (!hasNode(tree, id)) {
      return {};
    }
    const auto& node = tree.nodes[id];
    return node.folder ? measureLabel(tree, id, WorkbenchMeasure::Items) : itemsText(node.items);
  }

  std::weak_ordering compareNames(std::string_view left, std::string_view right) {
    std::size_t i = 0;
    std::size_t j = 0;
    // Each step advances i, j or both, so the loop runs at most left.size() + right.size() times.
    while (i < left.size() && j < right.size()) {
      if (isDigit(left[i]) && isDigit(right[j])) {
        if (const auto order = compareDigitRuns(left, i, right, j); order != 0) {
          return order;
        }
        continue;
      }
      if (const auto order = rank(left[i]) <=> rank(right[j]); order != 0) {
        return order;
      }
      ++i;
      ++j;
    }
    return (left.size() - i) <=> (right.size() - j);
  }
} // namespace luciole::workbench
