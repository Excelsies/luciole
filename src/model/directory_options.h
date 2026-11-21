#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace luciole {
  inline constexpr std::array<int, 9> kGridIconSizes{32, 48, 64, 80, 96, 128, 160, 192, 256};
  inline constexpr int kDefaultGridIconSize = 96;
  inline constexpr std::array<int, 8> kListIconSizes{16, 20, 24, 28, 32, 40, 48, 64};
  inline constexpr int kDefaultListIconSize = 32;
  inline constexpr int kMinSidebarWidth = 160;
  inline constexpr int kMaxSidebarWidth = 400;
  inline constexpr int kDefaultSidebarWidth = 206;
  enum class ViewMode : std::uint8_t { List, Grid, Miller, Workbench };
  // List and Workbench show the current folder as details rows under a column header.
  constexpr bool showsDetailsRows(ViewMode mode) { return mode == ViewMode::List || mode == ViewMode::Workbench; }
  // Ctrl with the wheel and Ctrl with Plus, Minus and 0 resize the mode's icons: Grid's, or List's rows, which
  // Workbench shares.
  constexpr bool zoomsByKeys(ViewMode mode) { return mode != ViewMode::Miller; }
  // The mode after history reaches a visit saved in `saved`: Column view and Workbench stay on.
  constexpr ViewMode viewModeAfterVisit(ViewMode current, bool columnView, ViewMode saved) {
    if (columnView) {
      return ViewMode::Miller;
    }
    return current == ViewMode::Workbench ? ViewMode::Workbench : saved;
  }
  // What sizes Workbench shapes and orders their entries.
  enum class WorkbenchMeasure : std::uint8_t { Bytes, Files, Items, Modified, Visits, Equal };
  // What colors Workbench shapes; Modified puts the newest change at the top of the ramp.
  enum class WorkbenchColor : std::uint8_t { Type, Bytes, Visits, Modified };
  // The Workbench part above Jump ahead in the left column.
  enum class WorkbenchInstrument : std::uint8_t { Nest, Iris, Atlas };
  // The Workbench part above the list.
  enum class WorkbenchRibbon : std::uint8_t { Flame, Flow, Ladder };
  inline constexpr std::size_t kWorkbenchRibbonCount = 3;
  // Where a file's preview is shown: in its column, with the place of Jump ahead taken when no column has room; in the
  // place of Jump ahead whatever the room; or in its column, and nowhere when no column has room.
  enum class WorkbenchPreviewPlace : std::uint8_t { JumpAheadWhenNarrow, JumpAheadAlways, HiddenWhenNarrow };
  // Saved Workbench pane sizes in pixels; the layout refuses larger ones.
  inline constexpr int kMaxWorkbenchPaneSize = 100000;
  inline constexpr std::size_t kWorkbenchColumnCount = 3;
  inline constexpr std::array<int, kWorkbenchColumnCount> kDefaultWorkbenchColumnWidths{260, 190, 140};
  inline constexpr int kMinWorkbenchColumnWidth = 140;
  inline constexpr int kMaxWorkbenchColumnWidth = 640;
  // A remembered Workbench list width is narrower than the 470 pixels the list keeps by default.
  inline constexpr int kMinWorkbenchListWidth = 320;
  inline constexpr int kMaxWorkbenchListWidth = 469;
  // The Workbench ancestor strips pane holds one to twelve named bands beside the "+N levels" band. A saved width is
  // in pixels at the default text size; the pane draws it times the text scale.
  inline constexpr int kMinWorkbenchStripsWidth = 70;
  inline constexpr int kMaxWorkbenchStripsWidth = 321;
  enum class ViewDensity : std::uint8_t { Compact, Comfortable };
  enum class FilenameTruncation : std::uint8_t { Middle, End };
  struct IconCaptions {
    bool size = false;
    bool type = false;
    bool modified = false;
    int count() const { return static_cast<int>(size) + static_cast<int>(type) + static_cast<int>(modified); }
    bool operator==(const IconCaptions&) const = default;
  };
  struct BrowsePresentation {
    ViewDensity density = ViewDensity::Compact;
    IconCaptions captions;
    bool showEmblems = true;
    FilenameTruncation filenameTruncation = FilenameTruncation::Middle;
    bool operator==(const BrowsePresentation&) const = default;
  };
  enum class ActivationMode : std::uint8_t { SingleClick, DoubleClick };
  enum class TypingMode : std::uint8_t { Search, Typeahead, Filter };
  // Complete column allocations, including the gap after their contents. Zero
  // is the automatic Name width; metadata always has an explicit allocation.
  inline constexpr std::size_t kDetailsColumnCount = 9;
  using DetailsColumnWidths = std::array<int, kDetailsColumnCount + 1>;
  inline constexpr DetailsColumnWidths kDefaultDetailsColumnWidths{0, 152, 118, 156, 156, 156, 128, 128, 112, 260};
  inline constexpr std::array<std::string_view, kDetailsColumnCount + 1> kDetailsColumnWidthKeys{
      "name_column_width",        "type_column_width",    "size_column_width",  "modified_column_width",
      "accessed_column_width",    "created_column_width", "owner_column_width", "group_column_width",
      "permissions_column_width", "location_column_width"
  };
  inline int minimumDetailsColumnWidth(std::size_t index) { return index == 0 ? 160 : 64; }
  inline int maximumDetailsColumnWidth(std::size_t index) { return index == 0 ? 16384 : 2048; }
  inline bool validDetailsColumnWidth(std::size_t index, int width) {
    return index < kDefaultDetailsColumnWidths.size()
        && ((index == 0 && width == 0)
            || (width >= minimumDetailsColumnWidth(index) && width <= maximumDetailsColumnWidth(index)));
  }
  struct DetailsColumnVisibility {
    bool type = true;
    bool size = true;
    bool modified = true;
    bool accessed = false, created = false, owner = false, group = false, permissions = false, location = false;
    bool operator==(const DetailsColumnVisibility&) const = default;
  };
  enum class DetailsColumn : std::uint8_t {
    Type,
    Size,
    Modified,
    Accessed,
    Created,
    Owner,
    Group,
    Permissions,
    Location
  };
  using DetailsColumnOrder = std::array<DetailsColumn, kDetailsColumnCount>;
  inline constexpr DetailsColumnOrder kDefaultDetailsColumnOrder{DetailsColumn::Type,     DetailsColumn::Size,
                                                                 DetailsColumn::Modified, DetailsColumn::Accessed,
                                                                 DetailsColumn::Created,  DetailsColumn::Owner,
                                                                 DetailsColumn::Group,    DetailsColumn::Permissions,
                                                                 DetailsColumn::Location};
  inline bool validDetailsColumnOrder(DetailsColumnOrder order) {
    std::ranges::sort(order);
    return order == kDefaultDetailsColumnOrder;
  }
  inline constexpr std::array<std::string_view, kDetailsColumnCount> kDetailsColumnNames{
      "type", "size", "modified", "accessed", "created", "owner", "group", "permissions", "location"
  };
  inline constexpr std::array<std::string_view, kDetailsColumnCount> kDetailsColumnLabels{
      "Type", "Size", "Modified", "Accessed", "Created", "Owner", "Group", "Permissions", "Location"
  };
  inline std::string_view detailsColumnName(DetailsColumn column) {
    const auto index = static_cast<std::size_t>(column);
    return index < kDetailsColumnNames.size() ? kDetailsColumnNames[index] : std::string_view{};
  }
  inline std::string_view detailsColumnLabel(DetailsColumn column) {
    const auto index = static_cast<std::size_t>(column);
    return index < kDetailsColumnLabels.size() ? kDetailsColumnLabels[index] : std::string_view{};
  }
  inline std::optional<DetailsColumn> detailsColumnFromName(std::string_view name) {
    for (const auto column : kDefaultDetailsColumnOrder) {
      if (detailsColumnName(column) == name) {
        return column;
      }
    }
    return std::nullopt;
  }
  inline bool& detailsColumnFlag(DetailsColumnVisibility& visibility, DetailsColumn column) {
    switch (column) {
    case DetailsColumn::Type:
      return visibility.type;
    case DetailsColumn::Size:
      return visibility.size;
    case DetailsColumn::Modified:
      return visibility.modified;
    case DetailsColumn::Accessed:
      return visibility.accessed;
    case DetailsColumn::Created:
      return visibility.created;
    case DetailsColumn::Owner:
      return visibility.owner;
    case DetailsColumn::Group:
      return visibility.group;
    case DetailsColumn::Permissions:
      return visibility.permissions;
    case DetailsColumn::Location:
      return visibility.location;
    }
    std::unreachable();
  }
  inline bool detailsColumnVisible(DetailsColumnVisibility visibility, DetailsColumn column) {
    return detailsColumnFlag(visibility, column);
  }
  inline DetailsColumnVisibility allDetailsColumns() {
    DetailsColumnVisibility result;
    for (const auto column : kDefaultDetailsColumnOrder) {
      detailsColumnFlag(result, column) = true;
    }
    return result;
  }
  inline std::optional<DetailsColumnOrder>
  moveDetailsColumn(DetailsColumnOrder order, DetailsColumn column, int direction, DetailsColumnVisibility visible) {
    if (!validDetailsColumnOrder(order)
        || (direction != -1 && direction != 1)
        || !detailsColumnVisible(visible, column)) {
      return std::nullopt;
    }
    auto* const found = std::ranges::find(order, column);
    if (found == order.end()) {
      return std::nullopt;
    }
    const auto source = found - order.begin();
    for (auto target = source + direction; target >= 0 && target < static_cast<std::ptrdiff_t>(order.size());
         target += direction) {
      if (detailsColumnVisible(visible, order[static_cast<std::size_t>(target)])) {
        std::swap(order[static_cast<std::size_t>(source)], order[static_cast<std::size_t>(target)]);
        return order;
      }
    }
    return std::nullopt;
  }
  inline std::optional<DetailsColumnOrder>
  insertDetailsColumnBefore(DetailsColumnOrder order, DetailsColumn column, std::optional<DetailsColumn> before) {
    if (!validDetailsColumnOrder(order)
        || !std::ranges::contains(order, column)
        || (before && !std::ranges::contains(order, *before))) {
      return std::nullopt;
    }
    if (before == column) {
      return order;
    }
    DetailsColumnOrder result{};
    std::size_t index = 0;
    for (const auto candidate : order) {
      if (candidate == column) {
        continue;
      }
      if (before == candidate) {
        result[index++] = column;
      }
      result[index++] = candidate;
    }
    if (!before) {
      result[index] = column;
    }
    return result;
  }
  enum class SortField : std::uint8_t {
    Name,
    Size,
    Modified,
    Type,
    Accessed,
    Created,
    Owner,
    Group,
    Permissions,
    Location
  };
  inline constexpr std::array<SortField, kDetailsColumnCount> kColumnSortFields{
      SortField::Type,  SortField::Size,  SortField::Modified,    SortField::Accessed, SortField::Created,
      SortField::Owner, SortField::Group, SortField::Permissions, SortField::Location
  };
  inline SortField detailsColumnSort(DetailsColumn column) {
    return kColumnSortFields[static_cast<std::size_t>(column)];
  }
  inline std::string_view sortFieldLabel(SortField field) {
    if (field == SortField::Name) {
      return "Name";
    }
    const auto* const found = std::ranges::find(kColumnSortFields, field);
    return found == kColumnSortFields.end() ? "Unknown" : kDetailsColumnLabels[found - kColumnSortFields.begin()];
  }
  struct SortOptions {
    SortField field = SortField::Name;
    bool descending = false;
    bool directoriesFirst = true;
    bool hiddenLast = false;
    bool operator==(const SortOptions&) const = default;
  };

  struct FilterOptions {
    bool showHidden = false;
    std::string text; // Literal substring of the display name, folded and normalized.
    bool showBackups = true;
    bool operator==(const FilterOptions&) const = default;
  };
} // namespace luciole
