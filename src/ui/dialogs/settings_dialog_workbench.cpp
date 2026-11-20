#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/dialogs/settings_dialog_rows.h"

#include <string>
#include <utility>

void SettingsDialog::addWorkbenchCard(std::function<void()> clearVisits) {
  using P = luciole::BrowserPreference;
  card("Workbench");
  addChoice(
      P::WorkbenchInstrument, "Instrument",
      {{"Nest", luciole::WorkbenchInstrument::Nest},
       {"Iris", luciole::WorkbenchInstrument::Iris},
       {"Atlas", luciole::WorkbenchInstrument::Atlas}}
  );
  addChoice(
      P::WorkbenchRibbon, "Ribbon",
      {{"Flame graph", luciole::WorkbenchRibbon::Flame},
       {"Flow", luciole::WorkbenchRibbon::Flow},
       {"Ladder", luciole::WorkbenchRibbon::Ladder}}
  );
  addBool(P::WorkbenchStrips, "Ancestor strips");
  addChoice(
      P::WorkbenchSizeBy, "Size by",
      {{"Size", luciole::WorkbenchMeasure::Bytes},
       {"Files", luciole::WorkbenchMeasure::Files},
       {"Items", luciole::WorkbenchMeasure::Items},
       {"Recent changes", luciole::WorkbenchMeasure::Modified},
       {"Visits", luciole::WorkbenchMeasure::Visits},
       {"Equal", luciole::WorkbenchMeasure::Equal}}
  );
  addChoice(
      P::WorkbenchColorBy, "Color by",
      {{"Type", luciole::WorkbenchColor::Type},
       {"Size", luciole::WorkbenchColor::Bytes},
       {"Visits", luciole::WorkbenchColor::Visits},
       {"Modified", luciole::WorkbenchColor::Modified}}
  );
  addFraction(P::WorkbenchEmphasis, "Emphasis", 1.0, 0.01);
  addChoice(
      P::WorkbenchPreviewPlace, "Preview placement",
      {{"Replace Jump ahead when it does not fit", luciole::WorkbenchPreviewPlace::JumpAheadWhenNarrow},
       {"Always replace Jump ahead", luciole::WorkbenchPreviewPlace::JumpAheadAlways},
       {"Hide when it does not fit", luciole::WorkbenchPreviewPlace::HiddenWhenNarrow}},
      "Where a file's preview is shown"
  );
  addBool(P::WorkbenchAudioVisualizer, "Audio visualizer");
  Choices<luciole::AudioVisualKind> visuals;
  for (const auto kind : luciole::kAudioVisualKinds) {
    visuals.emplace_back(std::string(luciole::audioVisualLabel(kind)), kind);
  }
  addChoice(P::WorkbenchAudioVisual, "Visual", visuals, "What a sound file shows while it plays");
  addBool(P::RecordVisits, "Record visits");
  m_section->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = 8.0F, .paddingV = 6.0F},
          ui::label({.text = "Recorded visits", .fontWeight = FontWeight::SemiBold, .flexGrow = 1.0F}),
          ui::button(
              {.out = &m_clearVisits,
               .text = "Clear visit history",
               .variant = ButtonVariant::Default,
               .minWidth = 180.0F,
               .maxWidth = 180.0F,
               .onClick = std::move(clearVisits)}
          )
      )
  );
}
