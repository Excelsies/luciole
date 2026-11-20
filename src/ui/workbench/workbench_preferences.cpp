#include "ui/workbench/workbench_preferences.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

namespace luciole {
  namespace {
    using P = BrowserPreference;

    constexpr std::array<PreferenceKey<int>, kWorkbenchRibbonCount> kRibbonHeightKeys{
        P::WorkbenchFlameHeight, P::WorkbenchFlowHeight, P::WorkbenchLadderHeight
    };
    constexpr std::array<PreferenceKey<int>, kWorkbenchColumnCount> kColumnWidthKeys{
        P::WorkbenchFirstColumnWidth, P::WorkbenchSecondColumnWidth, P::WorkbenchThirdColumnWidth
    };

    // A saved pane size; 0 and anything the settings refuse follow the view.
    std::optional<double> paneSize(int saved) {
      return saved > 0 && saved <= kMaxWorkbenchPaneSize ? std::optional<double>(saved) : std::nullopt;
    }

    int savedPaneSize(const std::optional<double>& size) {
      if (!size || !std::isfinite(*size) || *size <= 0) {
        return 0;
      }
      return static_cast<int>(std::clamp(std::round(*size), 1.0, static_cast<double>(kMaxWorkbenchPaneSize)));
    }

    int savedColumnWidth(double width, std::size_t column) {
      if (!std::isfinite(width)) {
        return kDefaultWorkbenchColumnWidths.at(column);
      }
      return static_cast<int>(std::clamp(
          std::round(width), static_cast<double>(kMinWorkbenchColumnWidth),
          static_cast<double>(kMaxWorkbenchColumnWidth)
      ));
    }

    int savedListWidth(const std::optional<double>& width) {
      if (!width || !std::isfinite(*width)) {
        return 0;
      }
      return static_cast<int>(std::clamp(
          std::round(*width), static_cast<double>(kMinWorkbenchListWidth), static_cast<double>(kMaxWorkbenchListWidth)
      ));
    }

    int savedStripsWidth(const std::optional<double>& width) {
      if (!width || !std::isfinite(*width)) {
        return 0;
      }
      return static_cast<int>(std::clamp(
          std::round(*width), static_cast<double>(kMinWorkbenchStripsWidth),
          static_cast<double>(kMaxWorkbenchStripsWidth)
      ));
    }

    template <class T> void edit(std::vector<OverrideEdit>& edits, PreferenceKey<T> key, T before, T after) {
      if (before != after) {
        edits.emplace_back(key, after);
      }
    }
  } // namespace

  WorkbenchGeometry workbenchGeometry(const WorkbenchSettings& settings) {
    WorkbenchGeometry geometry;
    geometry.panes.leftWidth = paneSize(settings.leftWidth);
    geometry.panes.instrumentHeight = paneSize(settings.instrumentHeight);
    for (std::size_t kind = 0; kind < kWorkbenchRibbonCount; ++kind) {
      geometry.panes.ribbonHeights.at(kind) = paneSize(settings.ribbonHeights.at(kind));
    }
    geometry.panes.collapsed = {
        .instrument = settings.instrumentCollapsed, .jump = settings.jumpCollapsed, .ribbon = settings.ribbonCollapsed
    };
    for (std::size_t column = 0; column < kWorkbenchColumnCount; ++column) {
      const int width = settings.columnWidths.at(column);
      const bool valid = width >= kMinWorkbenchColumnWidth && width <= kMaxWorkbenchColumnWidth;
      geometry.columns.columns.at(column) = valid ? width : kDefaultWorkbenchColumnWidths.at(column);
    }
    if (settings.listWidth >= kMinWorkbenchListWidth && settings.listWidth <= kMaxWorkbenchListWidth) {
      geometry.columns.list = settings.listWidth;
    }
    if (settings.stripsWidth >= kMinWorkbenchStripsWidth && settings.stripsWidth <= kMaxWorkbenchStripsWidth) {
      geometry.strips = settings.stripsWidth;
    }
    return geometry;
  }

  WorkbenchSettings withWorkbenchGeometry(WorkbenchSettings settings, const WorkbenchGeometry& geometry) {
    settings.leftWidth = savedPaneSize(geometry.panes.leftWidth);
    settings.instrumentHeight = savedPaneSize(geometry.panes.instrumentHeight);
    for (std::size_t kind = 0; kind < kWorkbenchRibbonCount; ++kind) {
      settings.ribbonHeights.at(kind) = savedPaneSize(geometry.panes.ribbonHeights.at(kind));
    }
    settings.instrumentCollapsed = geometry.panes.collapsed.instrument;
    settings.jumpCollapsed = geometry.panes.collapsed.jump;
    settings.ribbonCollapsed = geometry.panes.collapsed.ribbon;
    for (std::size_t column = 0; column < kWorkbenchColumnCount; ++column) {
      settings.columnWidths.at(column) = savedColumnWidth(geometry.columns.columns.at(column), column);
    }
    settings.listWidth = savedListWidth(geometry.columns.list);
    settings.stripsWidth = savedStripsWidth(geometry.strips);
    return settings;
  }

  std::vector<OverrideEdit> workbenchGeometryEdits(const WorkbenchSettings& before, const WorkbenchSettings& after) {
    std::vector<OverrideEdit> edits;
    edit(edits, P::WorkbenchLeftWidth, before.leftWidth, after.leftWidth);
    edit(edits, P::WorkbenchInstrumentHeight, before.instrumentHeight, after.instrumentHeight);
    for (std::size_t kind = 0; kind < kWorkbenchRibbonCount; ++kind) {
      edit(edits, kRibbonHeightKeys.at(kind), before.ribbonHeights.at(kind), after.ribbonHeights.at(kind));
    }
    edit(edits, P::WorkbenchInstrumentCollapsed, before.instrumentCollapsed, after.instrumentCollapsed);
    edit(edits, P::WorkbenchJumpCollapsed, before.jumpCollapsed, after.jumpCollapsed);
    edit(edits, P::WorkbenchRibbonCollapsed, before.ribbonCollapsed, after.ribbonCollapsed);
    for (std::size_t column = 0; column < kWorkbenchColumnCount; ++column) {
      edit(edits, kColumnWidthKeys.at(column), before.columnWidths.at(column), after.columnWidths.at(column));
    }
    edit(edits, P::WorkbenchListWidth, before.listWidth, after.listWidth);
    edit(edits, P::WorkbenchStripsWidth, before.stripsWidth, after.stripsWidth);
    return edits;
  }

  int workbenchPreviewSize(const WorkbenchSettings& settings) {
    return std::ranges::contains(kPreviewSizes, settings.previewSize) ? settings.previewSize : kDefaultPreviewSize;
  }

  int steppedPreviewSize(int size, int steps) {
    const auto* found = std::ranges::find(kPreviewSizes, size);
    if (found == kPreviewSizes.end()) {
      found = std::ranges::find(kPreviewSizes, kDefaultPreviewSize);
    }
    const auto last = static_cast<long long>(kPreviewSizes.size()) - 1;
    const auto index = std::clamp(static_cast<long long>(found - kPreviewSizes.begin()) + steps, 0LL, last);
    return kPreviewSizes.at(static_cast<std::size_t>(index));
  }

  WorkbenchChoices workbenchChoices(const BrowserSettings& settings, bool showHidden) {
    const auto& workbench = settings.workbench;
    return {
        .instrument = workbench.instrument,
        .ribbon = workbench.ribbon,
        .strips = workbench.strips,
        .layout =
            {.sizeBy = workbench.sizeBy,
             .colorBy = workbench.colorBy,
             .emphasis = workbench.emphasis,
             .showHidden = showHidden},
        .activation = settings.activationMode,
        .previewPlace = workbench.previewPlace,
    };
  }

  AudioVisualChoice audioVisualChoice(const WorkbenchSettings& settings) {
    return {.on = settings.audioVisualizer, .kind = settings.audioVisual};
  }

  std::vector<OverrideEdit> chooseAudioVisual(WorkbenchSettings& settings, const AudioVisualChoice& choice) {
    std::vector<OverrideEdit> edits;
    edit(edits, P::WorkbenchAudioVisualizer, std::exchange(settings.audioVisualizer, choice.on), choice.on);
    edit(edits, P::WorkbenchAudioVisual, std::exchange(settings.audioVisual, choice.kind), choice.kind);
    return edits;
  }

  std::vector<OverrideEdit> chooseMediaLoop(WorkbenchSettings& settings, bool loop) {
    std::vector<OverrideEdit> edits;
    edit(edits, P::WorkbenchMediaLoop, std::exchange(settings.mediaLoop, loop), loop);
    return edits;
  }
} // namespace luciole
