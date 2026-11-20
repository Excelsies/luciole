#pragma once

#include "config/preference_names.h"
#include "config/settings.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <tuple>
#include <type_traits>
#include <utility>

// The browser.* schema: TOML names, accepted values and the BrowserSettings field of every preference. Only
// settings.cpp includes this header.
namespace luciole::preference_schema {
  // How a preference is spelled in TOML and which values it accepts. Enums
  // are strings from `names`.
  template <class T> struct Rule {
    std::span<const Named<T>> names;
  };
  template <> struct Rule<bool> {};
  template <> struct Rule<std::string> {};
  template <> struct Rule<int> {
    bool (*accepts)(int);
    std::string_view expected;
  };
  template <> struct Rule<double> {
    double minimum;
    double maximum;
    std::string_view expected;
  };
  template <> struct Rule<luciole::DetailsColumnOrder> {};

  inline bool accepts(const Rule<std::string>& /*rule*/, const std::string& value) {
    return value.size() <= 4096 && !value.contains('\0') && value.find_first_of("\r\n") == std::string::npos;
  }
  inline bool accepts(const Rule<bool>& /*rule*/, bool /*value*/) { return true; }
  inline bool accepts(const Rule<int>& rule, int value) { return rule.accepts(value); }
  inline bool accepts(const Rule<double>& rule, double value) { return value >= rule.minimum && value <= rule.maximum; }
  inline bool accepts(const Rule<luciole::DetailsColumnOrder>& /*rule*/, const luciole::DetailsColumnOrder& order) {
    return luciole::validDetailsColumnOrder(order);
  }
  template <class T>
    requires std::is_enum_v<T>
  bool accepts(const Rule<T>& rule, T value) {
    return std::ranges::contains(rule.names, value, &Named<T>::value);
  }

  inline std::optional<std::string> decode(const Rule<std::string>& /*rule*/, const toml::node& node) {
    return node.value_exact<std::string>();
  }
  inline std::string expected(const Rule<std::string>& /*rule*/) {
    return "expected a single-line command of at most 4096 bytes";
  }
  inline void
  encode(toml::table& table, std::string_view key, const Rule<std::string>& /*rule*/, const std::string& value) {
    table.insert(key, value);
  }

  // Shape only; callers check accepts() as well.
  inline std::optional<bool> decode(const Rule<bool>& /*rule*/, const toml::node& node) {
    return node.value_exact<bool>();
  }
  inline std::optional<int> decode(const Rule<int>& /*rule*/, const toml::node& node) {
    const auto number = node.value_exact<std::int64_t>();
    return number && std::in_range<int>(*number) ? std::optional<int>(static_cast<int>(*number)) : std::nullopt;
  }
  // Accepts integers too, so `1` reads as 1.0.
  inline std::optional<double> decode(const Rule<double>& /*rule*/, const toml::node& node) {
    return node.is_number() ? node.value<double>() : std::nullopt;
  }
  inline std::optional<luciole::DetailsColumnOrder>
  decode(const Rule<luciole::DetailsColumnOrder>& /*rule*/, const toml::node& node) {
    const auto* columns = node.as_array();
    luciole::DetailsColumnOrder order{};
    if (columns == nullptr || columns->size() < 3 || columns->size() > order.size()) {
      return std::nullopt;
    }
    std::array<bool, luciole::kDetailsColumnCount> seen{};
    std::size_t index = 0;
    for (const auto& item : *columns) {
      const auto* name = item.as_string();
      const auto column = name ? luciole::detailsColumnFromName(name->get()) : std::nullopt;
      if (!column || seen[static_cast<std::size_t>(*column)]) {
        return std::nullopt;
      }
      seen[static_cast<std::size_t>(*column)] = true;
      order[index++] = *column;
    }
    if (!seen[0] || !seen[1] || !seen[2]) {
      return std::nullopt;
    }
    for (const auto column : luciole::kDefaultDetailsColumnOrder) {
      if (!seen[static_cast<std::size_t>(column)]) {
        order[index++] = column;
      }
    }
    return order;
  }
  template <class T>
    requires std::is_enum_v<T>
  std::optional<T> decode(const Rule<T>& rule, const toml::node& node) {
    const auto* text = node.as_string();
    if (text == nullptr) {
      return std::nullopt;
    }
    const auto found = std::ranges::find(rule.names, std::string_view(text->get()), &Named<T>::name);
    return found != rule.names.end() ? std::optional<T>(found->value) : std::nullopt;
  }

  inline std::string expected(const Rule<bool>& /*rule*/) { return "expected a boolean"; }
  inline std::string expected(const Rule<int>& rule) { return std::string(rule.expected); }
  inline std::string expected(const Rule<double>& rule) { return std::string(rule.expected); }
  inline std::string expected(const Rule<luciole::DetailsColumnOrder>& /*rule*/) {
    return "expected unique column names including 'type', 'size', and 'modified'; optional columns may be omitted";
  }
  template <class T>
    requires std::is_enum_v<T>
  std::string expected(const Rule<T>& rule) {
    std::string text = "expected ";
    for (std::size_t index = 0; index < rule.names.size(); ++index) {
      if (index > 0) {
        text += rule.names.size() == 2 ? " or " : index + 1 == rule.names.size() ? ", or " : ", ";
      }
      text += std::format("'{}'", rule.names[index].name);
    }
    return text;
  }

  inline void encode(toml::table& table, std::string_view key, const Rule<bool>& /*rule*/, bool value) {
    table.insert(key, value);
  }
  inline void encode(toml::table& table, std::string_view key, const Rule<int>& /*rule*/, int value) {
    table.insert(key, value);
  }
  inline void encode(toml::table& table, std::string_view key, const Rule<double>& /*rule*/, double value) {
    table.insert(key, value);
  }
  inline void encode(
      toml::table& table, std::string_view key, const Rule<luciole::DetailsColumnOrder>& /*rule*/,
      const luciole::DetailsColumnOrder& order
  ) {
    toml::array columns;
    for (const auto column : order) {
      columns.push_back(luciole::detailsColumnName(column));
    }
    table.insert(key, std::move(columns));
  }
  template <class T>
    requires std::is_enum_v<T>
  void encode(toml::table& table, std::string_view key, const Rule<T>& rule, T value) {
    table.insert(key, std::ranges::find(rule.names, value, &Named<T>::value)->name);
  }

  template <class T> struct Descriptor {
    using Value = T;
    luciole::PreferenceKey<T> preference;
    std::string_view key; // Field name inside [browser].
    T& (*field)(BrowserSettings&);
    Rule<T> rule;
  };
  template <class T>
  constexpr Descriptor<T> describe(
      luciole::PreferenceKey<T> preference, std::string_view key, std::type_identity_t<T>& (*field)(BrowserSettings&),
      Rule<T> rule = {}
  ) {
    return {.preference = preference, .key = key, .field = field, .rule = rule};
  }
  template <int Minimum, int Maximum> bool between(int value) { return value >= Minimum && value <= Maximum; }
  template <int Minimum, int Maximum> bool zeroOrBetween(int value) {
    return value == 0 || between<Minimum, Maximum>(value);
  }
  template <std::size_t Index> bool columnWidth(int value) { return luciole::validDetailsColumnWidth(Index, value); }

  using P = luciole::BrowserPreference;
  using S = BrowserSettings;
  // Every browser.* setting, in BrowserPreferenceId order. Defaults are BrowserSettings' initializers.
  inline constexpr std::tuple kPreferences{
      describe(
          P::ActivationMode, "activation_mode", [](S& s) -> auto& { return s.activationMode; }, {kActivationModes}
      ),
      describe(P::TypingMode, "typing_mode", [](S& s) -> auto& { return s.typingMode; }, {kTypingModes}),
      describe(
          P::ConfirmPermanentDeletion, "confirm_permanent_deletion",
          [](S& s) -> auto& { return s.confirmPermanentDeletion; }
      ),
      describe(P::SwitchToNewTab, "switch_to_new_tab", [](S& s) -> auto& { return s.switchToNewTab; }),
      describe(P::RestoreLastWindow, "restore_last_window", [](S& s) -> auto& { return s.restoreLastWindow; }),
      describe(P::ShowHidden, "show_hidden", [](S& s) -> auto& { return s.showHidden; }),
      describe(P::ShowBackups, "show_backups", [](S& s) -> auto& { return s.showBackups; }),
      describe(P::AlwaysShowTabBar, "always_show_tab_bar", [](S& s) -> auto& { return s.alwaysShowTabBar; }),
      describe(P::AlwaysShowStatusBar, "always_show_status_bar", [](S& s) -> auto& { return s.alwaysShowStatusBar; }),
      describe(P::CompactMenus, "compact_menus", [](S& s) -> auto& { return s.compactMenus; }),
      describe(
          P::BackgroundOpacity, "background_opacity", [](S& s) -> auto& { return s.backgroundOpacity; },
          {0.0, 1.0, "expected a number from 0 to 1"}
      ),
      describe(P::SortBy, "sort_by", [](S& s) -> auto& { return s.sort.field; }, {kSortFields}),
      describe(P::SortDescending, "sort_descending", [](S& s) -> auto& { return s.sort.descending; }),
      describe(P::DirectoriesFirst, "directories_first", [](S& s) -> auto& { return s.sort.directoriesFirst; }),
      describe(P::ViewMode, "view_mode", [](S& s) -> auto& { return s.viewMode; }, {kViewModes}),
      describe(
          P::GridIconSize, "grid_icon_size", [](S& s) -> auto& { return s.gridIconSize; },
          {[](int value) { return std::ranges::contains(luciole::kGridIconSizes, value); },
           "expected integer icon size 32, 48, 64, 80, 96, 128, 160, 192, or 256"}
      ),
      describe(P::Density, "density", [](S& s) -> auto& { return s.presentation.density; }, {kDensities}),
      describe(
          P::FilenameTruncation, "filename_truncation", [](S& s) -> auto& { return s.presentation.filenameTruncation; },
          {kFilenameTruncations}
      ),
      describe(P::ShowSizeCaption, "show_size_caption", [](S& s) -> auto& { return s.presentation.captions.size; }),
      describe(P::ShowTypeCaption, "show_type_caption", [](S& s) -> auto& { return s.presentation.captions.type; }),
      describe(
          P::ShowModifiedCaption, "show_modified_caption",
          [](S& s) -> auto& { return s.presentation.captions.modified; }
      ),
      describe(P::ShowEmblems, "show_emblems", [](S& s) -> auto& { return s.presentation.showEmblems; }),
      describe(
          P::SidebarWidth, "sidebar_width", [](S& s) -> auto& { return s.sidebarWidth; },
          {between<luciole::kMinSidebarWidth, luciole::kMaxSidebarWidth>,
           "expected integer sidebar width from 160 to 400"}
      ),
      describe(P::ShowSidebar, "show_sidebar", [](S& s) -> auto& { return s.showSidebar; }),
      describe(P::SidebarTree, "sidebar_tree", [](S& s) -> auto& { return s.sidebarTree; }),
      describe(P::ShowDrives, "show_drives", [](S& s) -> auto& { return s.showDrives; }),
      describe(P::ExpandableFolders, "expandable_folders", [](S& s) -> auto& { return s.expandableFolders; }),
      describe(P::ShowPreview, "show_preview", [](S& s) -> auto& { return s.showPreview; }),
      describe(P::ChooserShowPreview, "chooser_show_preview", [](S& s) -> auto& { return s.chooserShowPreview; }),
      describe(
          P::PreviewWidth, "preview_width", [](S& s) -> auto& { return s.previewWidth; },
          {between<208, 800>, "expected integer preview width from 208 to 800"}
      ),
      describe(
          P::MillerColumnWidth, "miller_column_width", [](S& s) -> auto& { return s.millerColumnWidth; },
          {between<180, 800>, "expected integer column width from 180 to 800"}
      ),
      describe(P::MillerShowParents, "miller_show_parents", [](S& s) -> auto& { return s.millerShowParents; }),
      describe(P::ShowTypeColumn, "show_type_column", [](S& s) -> auto& { return s.columns.type; }),
      describe(P::ShowSizeColumn, "show_size_column", [](S& s) -> auto& { return s.columns.size; }),
      describe(P::ShowModifiedColumn, "show_modified_column", [](S& s) -> auto& { return s.columns.modified; }),
      describe(P::ShowAccessedColumn, "show_accessed_column", [](S& s) -> auto& { return s.columns.accessed; }),
      describe(P::ShowCreatedColumn, "show_created_column", [](S& s) -> auto& { return s.columns.created; }),
      describe(P::ShowOwnerColumn, "show_owner_column", [](S& s) -> auto& { return s.columns.owner; }),
      describe(P::ShowGroupColumn, "show_group_column", [](S& s) -> auto& { return s.columns.group; }),
      describe(
          P::ShowPermissionsColumn, "show_permissions_column", [](S& s) -> auto& { return s.columns.permissions; }
      ),
      describe(P::ShowLocationColumn, "show_location_column", [](S& s) -> auto& { return s.columns.location; }),
      describe(P::ColumnOrder, "column_order", [](S& s) -> auto& { return s.columnOrder; }),
      describe(
          P::NameColumnWidth, luciole::kDetailsColumnWidthKeys[0], [](S& s) -> auto& { return s.columnWidths[0]; },
          {columnWidth<0>, "expected 0 (automatic) or an integer width from 160 to 16384"}
      ),
      describe(
          P::TypeColumnWidth, luciole::kDetailsColumnWidthKeys[1], [](S& s) -> auto& { return s.columnWidths[1]; },
          {columnWidth<1>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::SizeColumnWidth, luciole::kDetailsColumnWidthKeys[2], [](S& s) -> auto& { return s.columnWidths[2]; },
          {columnWidth<2>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::ModifiedColumnWidth, luciole::kDetailsColumnWidthKeys[3], [](S& s) -> auto& { return s.columnWidths[3]; },
          {columnWidth<3>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::AccessedColumnWidth, luciole::kDetailsColumnWidthKeys[4], [](S& s) -> auto& { return s.columnWidths[4]; },
          {columnWidth<4>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::CreatedColumnWidth, luciole::kDetailsColumnWidthKeys[5], [](S& s) -> auto& { return s.columnWidths[5]; },
          {columnWidth<5>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::OwnerColumnWidth, luciole::kDetailsColumnWidthKeys[6], [](S& s) -> auto& { return s.columnWidths[6]; },
          {columnWidth<6>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::GroupColumnWidth, luciole::kDetailsColumnWidthKeys[7], [](S& s) -> auto& { return s.columnWidths[7]; },
          {columnWidth<7>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::PermissionsColumnWidth, luciole::kDetailsColumnWidthKeys[8],
          [](S& s) -> auto& { return s.columnWidths[8]; }, {columnWidth<8>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::LocationColumnWidth, luciole::kDetailsColumnWidthKeys[9], [](S& s) -> auto& { return s.columnWidths[9]; },
          {columnWidth<9>, "expected an integer width from 64 to 2048"}
      ),
      describe(
          P::ThumbnailPolicy, "thumbnail_policy", [](S& s) -> auto& { return s.thumbnails.policy; },
          {kThumbnailPolicies}
      ),
      describe(
          P::ThumbnailMaxFileMib, "thumbnail_max_file_mib", [](S& s) -> auto& { return s.thumbnails.maxFileMiB; },
          {between<1, 1024>, "expected an integer from 1 to 1024"}
      ),
      describe(
          P::ThumbnailCacheMib, "thumbnail_cache_mib", [](S& s) -> auto& { return s.thumbnails.cacheMiB; },
          {between<16, 1024>, "expected an integer from 16 to 1024"}
      ),
      describe(P::TerminalCommand, "terminal_command", [](S& s) -> auto& { return s.terminalCommand; }),
      describe(P::RecordVisits, "record_visits", [](S& s) -> auto& { return s.recordVisits; }),
      describe(
          P::WorkbenchInstrument, "workbench_instrument", [](S& s) -> auto& { return s.workbench.instrument; },
          {kWorkbenchInstruments}
      ),
      describe(
          P::WorkbenchRibbon, "workbench_ribbon", [](S& s) -> auto& { return s.workbench.ribbon; }, {kWorkbenchRibbons}
      ),
      describe(P::WorkbenchStrips, "workbench_strips", [](S& s) -> auto& { return s.workbench.strips; }),
      describe(
          P::WorkbenchSizeBy, "workbench_size_by", [](S& s) -> auto& { return s.workbench.sizeBy; },
          {kWorkbenchMeasures}
      ),
      describe(
          P::WorkbenchColorBy, "workbench_color_by", [](S& s) -> auto& { return s.workbench.colorBy; },
          {kWorkbenchColors}
      ),
      describe(
          P::WorkbenchEmphasis, "workbench_emphasis", [](S& s) -> auto& { return s.workbench.emphasis; },
          {0.0, 1.0, "expected a number from 0 to 1"}
      ),
      describe(
          P::WorkbenchLeftWidth, "workbench_left_width", [](S& s) -> auto& { return s.workbench.leftWidth; },
          {between<0, luciole::kMaxWorkbenchPaneSize>, "expected 0 (automatic) or an integer width from 1 to 100000"}
      ),
      describe(
          P::WorkbenchInstrumentHeight, "workbench_instrument_height",
          [](S& s) -> auto& { return s.workbench.instrumentHeight; },
          {between<0, luciole::kMaxWorkbenchPaneSize>, "expected 0 (automatic) or an integer height from 1 to 100000"}
      ),
      describe(
          P::WorkbenchFlameHeight, "workbench_flame_height", [](S& s) -> auto& { return s.workbench.ribbonHeights[0]; },
          {between<0, luciole::kMaxWorkbenchPaneSize>, "expected 0 (automatic) or an integer height from 1 to 100000"}
      ),
      describe(
          P::WorkbenchFlowHeight, "workbench_flow_height", [](S& s) -> auto& { return s.workbench.ribbonHeights[1]; },
          {between<0, luciole::kMaxWorkbenchPaneSize>, "expected 0 (automatic) or an integer height from 1 to 100000"}
      ),
      describe(
          P::WorkbenchLadderHeight, "workbench_ladder_height",
          [](S& s) -> auto& { return s.workbench.ribbonHeights[2]; },
          {between<0, luciole::kMaxWorkbenchPaneSize>, "expected 0 (automatic) or an integer height from 1 to 100000"}
      ),
      describe(
          P::WorkbenchInstrumentCollapsed, "workbench_instrument_collapsed",
          [](S& s) -> auto& { return s.workbench.instrumentCollapsed; }
      ),
      describe(
          P::WorkbenchJumpCollapsed, "workbench_jump_collapsed", [](S& s) -> auto& { return s.workbench.jumpCollapsed; }
      ),
      describe(
          P::WorkbenchRibbonCollapsed, "workbench_ribbon_collapsed",
          [](S& s) -> auto& { return s.workbench.ribbonCollapsed; }
      ),
      describe(
          P::WorkbenchFirstColumnWidth, "workbench_first_column_width",
          [](S& s) -> auto& { return s.workbench.columnWidths[0]; },
          {between<luciole::kMinWorkbenchColumnWidth, luciole::kMaxWorkbenchColumnWidth>,
           "expected an integer width from 140 to 640"}
      ),
      describe(
          P::WorkbenchSecondColumnWidth, "workbench_second_column_width",
          [](S& s) -> auto& { return s.workbench.columnWidths[1]; },
          {between<luciole::kMinWorkbenchColumnWidth, luciole::kMaxWorkbenchColumnWidth>,
           "expected an integer width from 140 to 640"}
      ),
      describe(
          P::WorkbenchThirdColumnWidth, "workbench_third_column_width",
          [](S& s) -> auto& { return s.workbench.columnWidths[2]; },
          {between<luciole::kMinWorkbenchColumnWidth, luciole::kMaxWorkbenchColumnWidth>,
           "expected an integer width from 140 to 640"}
      ),
      describe(
          P::WorkbenchListWidth, "workbench_list_width", [](S& s) -> auto& { return s.workbench.listWidth; },
          {zeroOrBetween<luciole::kMinWorkbenchListWidth, luciole::kMaxWorkbenchListWidth>,
           "expected 0 (none) or an integer width from 320 to 469"}
      ),
      describe(
          P::WorkbenchStripsWidth, "workbench_strips_width", [](S& s) -> auto& { return s.workbench.stripsWidth; },
          {zeroOrBetween<luciole::kMinWorkbenchStripsWidth, luciole::kMaxWorkbenchStripsWidth>,
           "expected 0 (the default) or an integer width from 70 to 321"}
      ),
      describe(
          P::WorkbenchPreviewSize, "workbench_preview_size", [](S& s) -> auto& { return s.workbench.previewSize; },
          {[](int value) { return std::ranges::contains(luciole::kPreviewSizes, value); },
           "expected integer percent 70, 80, 90, 100, 110, 125, 150, 175, 200, 250, or 300"}
      ),
      describe(
          P::WorkbenchAudioVisualizer, "workbench_audio_visualizer",
          [](S& s) -> auto& { return s.workbench.audioVisualizer; }
      ),
      describe(
          P::WorkbenchAudioVisual, "workbench_audio_visual", [](S& s) -> auto& { return s.workbench.audioVisual; },
          {kAudioVisuals}
      ),
      describe(P::WorkbenchMediaLoop, "workbench_media_loop", [](S& s) -> auto& { return s.workbench.mediaLoop; }),
      describe(
          P::WorkbenchPreviewPlace, "workbench_preview_place", [](S& s) -> auto& { return s.workbench.previewPlace; },
          {kWorkbenchPreviewPlaces}
      ),
  };
  inline constexpr std::size_t kPreferenceCount = std::tuple_size_v<decltype(kPreferences)>;
  static_assert(kPreferenceCount == std::to_underlying(luciole::BrowserPreferenceId::Count));
  static_assert(
      []<std::size_t... I>(std::index_sequence<I...>) {
        return ((std::get<I>(kPreferences).preference.id() == static_cast<luciole::BrowserPreferenceId>(I)) && ...);
      }(std::make_index_sequence<kPreferenceCount>{}),
      "kPreferences must follow BrowserPreferenceId order"
  );

  // Calls `visit` with each descriptor index as a std::integral_constant.
  template <class Visit> void forEachPreference(Visit&& visit) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (visit(std::integral_constant<std::size_t, I>{}), ...);
    }(std::make_index_sequence<kPreferenceCount>{});
  }
  template <class Visit> void visitPreference(luciole::BrowserPreferenceId id, Visit&& visit) {
    forEachPreference([&](auto index) {
      if (index == std::to_underlying(id)) {
        visit(index);
      }
    });
  }

  template <class T> std::string preferenceKey(const Descriptor<T>& descriptor) {
    return std::format("browser.{}", descriptor.key);
  }
} // namespace luciole::preference_schema
