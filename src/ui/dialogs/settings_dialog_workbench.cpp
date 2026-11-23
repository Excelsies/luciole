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
      {{"Flame Graph", luciole::WorkbenchRibbon::Flame},
       {"Flow", luciole::WorkbenchRibbon::Flow},
       {"Ladder", luciole::WorkbenchRibbon::Ladder}}
  );
  addBool(P::WorkbenchStrips, "Ancestor Strips");
  addChoice(
      P::WorkbenchSizeBy, "Size by",
      {{"Size", luciole::WorkbenchMeasure::Bytes},
       {"Files", luciole::WorkbenchMeasure::Files},
       {"Items", luciole::WorkbenchMeasure::Items},
       {"Recent Changes", luciole::WorkbenchMeasure::Modified},
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
      P::WorkbenchPreviewPlace, "Preview Placement",
      {{"Replace Jump Ahead When It Does Not Fit", luciole::WorkbenchPreviewPlace::JumpAheadWhenNarrow},
       {"Always Replace Jump Ahead", luciole::WorkbenchPreviewPlace::JumpAheadAlways},
       {"Hide When It Does Not Fit", luciole::WorkbenchPreviewPlace::HiddenWhenNarrow}},
      "Where a file's preview is shown"
  );
  addBool(P::WorkbenchAudioVisualizer, "Audio Visualizer");
  Choices<luciole::AudioVisualKind> visuals;
  for (const auto kind : luciole::kAudioVisualKinds) {
    visuals.emplace_back(std::string(luciole::audioVisualLabel(kind)), kind);
  }
  addChoice(P::WorkbenchAudioVisual, "Visual", visuals, "What a sound file shows while it plays");
  addBool(P::RecordVisits, "Record Visits");
  m_section->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = 8.0F, .paddingV = 6.0F, .minHeight = Style::controlHeight},
          ui::label({.text = "Recorded Visits", .fontWeight = FontWeight::SemiBold, .flexGrow = 1.0F}),
          ui::button(
              {.out = &m_clearVisits,
               .text = "Clear Visit History",
               .variant = ButtonVariant::Default,
               .minWidth = 180.0F, // no maximum: the text grows with the system font
               .onClick = std::move(clearVisits)}
          )
      )
  );
}
