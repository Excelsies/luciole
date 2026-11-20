#pragma once

#include "config/settings.h"
#include "ui/controls/audio_visual_view.h"
#include "ui/workbench/workbench_view.h"

#include <vector>

// The Workbench settings as the view reads them, and the view's geometry as settings edits.
namespace luciole {
  // The saved pane sizes and column widths as the view's geometry. A size of 0 follows the view, and so does any size
  // the settings would refuse; a column width outside 140 to 640 is that column's default, and a list width outside
  // 320 to 469 or a strips pane width outside 70 to 321 is none. Settings keep no expanded flag, so the instrument
  // comes out not expanded.
  WorkbenchGeometry workbenchGeometry(const WorkbenchSettings& settings);

  // `settings` with the sizes of `geometry` in whole pixels, held to the ranges the settings accept. A size that
  // follows the view, or one that is not finite, is saved as 0; a column width that is not finite is its default.
  WorkbenchSettings withWorkbenchGeometry(WorkbenchSettings settings, const WorkbenchGeometry& geometry);

  // One edit per geometry field that differs between `before` and `after`, in BrowserPreferenceId order.
  std::vector<OverrideEdit> workbenchGeometryEdits(const WorkbenchSettings& before, const WorkbenchSettings& after);

  // The saved preview size in percent: one of kPreviewSizes, or kDefaultPreviewSize for any other value.
  int workbenchPreviewSize(const WorkbenchSettings& settings);

  // The preview size `steps` steps of kPreviewSizes away from `size`, held at the smallest and largest; a size off
  // the steps counts as kDefaultPreviewSize.
  int steppedPreviewSize(int size, int steps);

  // The choices the view applies; `showHidden` is the page's own setting for hidden entries.
  WorkbenchChoices workbenchChoices(const BrowserSettings& settings, bool showHidden);

  // The sound visualizer's choice the settings hold.
  AudioVisualChoice audioVisualChoice(const WorkbenchSettings& settings);
  // Sets `choice` in `settings` and returns one edit for each part that changed: the visualizer first, then the
  // visual.
  std::vector<OverrideEdit> chooseAudioVisual(WorkbenchSettings& settings, const AudioVisualChoice& choice);
  // Sets the loop switch in `settings` and returns the edit for it, or none when it did not change.
  std::vector<OverrideEdit> chooseMediaLoop(WorkbenchSettings& settings, bool loop);
} // namespace luciole
