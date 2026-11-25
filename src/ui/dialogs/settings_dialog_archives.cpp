#include "fs/archive_container_types.h"
#include "ui/builders.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/dialogs/settings_dialog_rows.h"

#include <memory>
#include <string>
#include <vector>

void SettingsDialog::addArchiveContainers() {
  using P = luciole::BrowserPreference;
  auto toggles = std::make_shared<std::vector<Toggle*>>(luciole::kArchiveContainerCount, nullptr);
  // The page-level description sits above the cards, since it covers every one.
  m_pages.back()->addChild(
      ui::label(
          {.text = "A type that is switched on opens in the archive browser instead of its default application.",
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 3}
      )
  );
  for (std::size_t family = 0; family < luciole::kArchiveFamilyCount; ++family) {
    const auto kind = static_cast<luciole::ArchiveFamily>(family);
    card(std::string(luciole::archiveFamilyLabel(kind)));
    for (std::size_t index = 0; index < luciole::kArchiveContainerCount; ++index) {
      const auto& type = luciole::kArchiveContainerTypes[index];
      if (type.family != kind) {
        continue;
      }
      Toggle* control = nullptr;
      (*toggles)[index] = nullptr;
      auto row = ui::row(
          {.justify = FlexJustify::End, .width = 180.0F},
          ui::toggle({.out = &control, .onChange = [this, index](bool on) {
                        auto next = m_current.openAsArchive;
                        next.set(index, on);
                        send(P::OpenAsArchive, std::optional(next));
                      }})
      );
      (*toggles)[index] = control;
      m_section->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = 8.0F, .paddingV = kRowPaddingV, .minHeight = Style::controlHeight},
              ui::label(
                  {.text = std::string(type.label), .fontWeight = FontWeight::SemiBold, .maxLines = 2, .flexGrow = 1.0F}
              ),
              std::move(row)
          )
      );
    }
  }
  m_rows[P::OpenAsArchive.id()] = [this, toggles] {
    for (std::size_t index = 0; index < toggles->size(); ++index) {
      (*toggles)[index]->setCheckedImmediate(m_current.openAsArchive.test(index));
    }
  };
}
