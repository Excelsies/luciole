#pragma once

#include "config/settings.h"

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

// The TOML words of each enum preference. Only preference_schema.h includes this header.
namespace luciole::preference_schema {
  template <class T> struct Named {
    T value;
    std::string_view name;
  };
  inline constexpr auto kActivationModes = std::to_array<Named<luciole::ActivationMode>>(
      {{luciole::ActivationMode::SingleClick, "single_click"}, {luciole::ActivationMode::DoubleClick, "double_click"}}
  );
  inline constexpr auto kTypingModes = std::to_array<Named<luciole::TypingMode>>(
      {{luciole::TypingMode::Search, "search"},
       {luciole::TypingMode::Typeahead, "typeahead"},
       {luciole::TypingMode::Filter, "filter"}}
  );
  inline constexpr auto kDropActions = std::to_array<Named<luciole::DropAction>>(
      {{luciole::DropAction::Menu, "menu"}, {luciole::DropAction::Copy, "copy"}, {luciole::DropAction::Move, "move"}}
  );
  inline constexpr auto kSortFields = std::to_array<Named<luciole::SortField>>(
      {{luciole::SortField::Name, "name"},
       {luciole::SortField::Size, "size"},
       {luciole::SortField::Modified, "modified"},
       {luciole::SortField::Type, "type"},
       {luciole::SortField::Accessed, "accessed"},
       {luciole::SortField::Created, "created"},
       {luciole::SortField::Owner, "owner"},
       {luciole::SortField::Group, "group"},
       {luciole::SortField::Permissions, "permissions"},
       {luciole::SortField::Location, "location"}}
  );
  inline constexpr auto kViewModes = std::to_array<Named<luciole::ViewMode>>(
      {{luciole::ViewMode::List, "list"},
       {luciole::ViewMode::Grid, "grid"},
       {luciole::ViewMode::Miller, "miller"},
       {luciole::ViewMode::Workbench, "workbench"}}
  );
  // File pickers have no Columns view: it enters folders on selection, which choosing a folder needs. Nor Workbench.
  inline constexpr auto kChooserViewModes =
      std::to_array<Named<luciole::ViewMode>>({{luciole::ViewMode::List, "list"}, {luciole::ViewMode::Grid, "grid"}});
  inline constexpr auto kChooserStartFolders = std::to_array<Named<luciole::ChooserStartFolder>>(
      {{luciole::ChooserStartFolder::Home, "home"}, {luciole::ChooserStartFolder::Last, "last"}}
  );
  inline constexpr auto kDensities = std::to_array<Named<luciole::ViewDensity>>(
      {{luciole::ViewDensity::Compact, "compact"}, {luciole::ViewDensity::Comfortable, "comfortable"}}
  );
  inline constexpr auto kFilenameTruncations = std::to_array<Named<luciole::FilenameTruncation>>(
      {{luciole::FilenameTruncation::Middle, "middle"}, {luciole::FilenameTruncation::End, "end"}}
  );
  inline constexpr auto kThumbnailPolicies = std::to_array<Named<luciole::ThumbnailPolicy>>(
      {{luciole::ThumbnailPolicy::Never, "never"},
       {luciole::ThumbnailPolicy::Local, "local"},
       {luciole::ThumbnailPolicy::All, "all"}}
  );
  inline constexpr auto kWorkbenchInstruments = std::to_array<Named<luciole::WorkbenchInstrument>>(
      {{luciole::WorkbenchInstrument::Nest, "nest"},
       {luciole::WorkbenchInstrument::Iris, "iris"},
       {luciole::WorkbenchInstrument::Atlas, "atlas"}}
  );
  inline constexpr auto kWorkbenchRibbons = std::to_array<Named<luciole::WorkbenchRibbon>>(
      {{luciole::WorkbenchRibbon::Flame, "flame"},
       {luciole::WorkbenchRibbon::Flow, "flow"},
       {luciole::WorkbenchRibbon::Ladder, "ladder"}}
  );
  inline constexpr auto kWorkbenchPreviewPlaces = std::to_array<Named<luciole::WorkbenchPreviewPlace>>(
      {{luciole::WorkbenchPreviewPlace::JumpAheadWhenNarrow, "jump_ahead_when_narrow"},
       {luciole::WorkbenchPreviewPlace::JumpAheadAlways, "jump_ahead_always"},
       {luciole::WorkbenchPreviewPlace::HiddenWhenNarrow, "hidden_when_narrow"}}
  );
  inline constexpr auto kWorkbenchMeasures = std::to_array<Named<luciole::WorkbenchMeasure>>(
      {{luciole::WorkbenchMeasure::Bytes, "size"},
       {luciole::WorkbenchMeasure::Files, "files"},
       {luciole::WorkbenchMeasure::Items, "items"},
       {luciole::WorkbenchMeasure::Modified, "modified"},
       {luciole::WorkbenchMeasure::Visits, "visits"},
       {luciole::WorkbenchMeasure::Equal, "equal"}}
  );
  inline constexpr auto kWorkbenchColors = std::to_array<Named<luciole::WorkbenchColor>>(
      {{luciole::WorkbenchColor::Type, "type"},
       {luciole::WorkbenchColor::Bytes, "size"},
       {luciole::WorkbenchColor::Visits, "visits"},
       {luciole::WorkbenchColor::Modified, "modified"}}
  );
  inline constexpr auto kAudioVisuals = []<std::size_t... I>(std::index_sequence<I...>) {
    return std::array{Named{luciole::kAudioVisualKinds[I], luciole::kAudioVisualNames[I]}...};
  }(std::make_index_sequence<luciole::kAudioVisualCount>{});
} // namespace luciole::preference_schema
