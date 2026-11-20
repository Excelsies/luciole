#pragma once

#include "ui/builders.h"
#include "ui/controls/select.h"
#include "ui/dialogs/settings_dialog.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

// The row builders of SettingsDialog, shared by the pages and the Workbench card.
// Like Noctalia's settings rows: every row is at least one control tall, so toggle, slider, dropdown and stepper
// rows share one height whatever their control measures; descriptions grow a row past it.
inline constexpr float kRowPaddingV = 2.0F;

template <class T>
void SettingsDialog::addRow(
    luciole::PreferenceKey<T> key, const std::string& title, std::function<void(const T&)> show,
    std::unique_ptr<Node> control, std::string subtitle
) {
  auto titleLabel = ui::label({.text = title, .fontWeight = FontWeight::SemiBold, .maxLines = 2, .flexGrow = 1.0F});
  std::unique_ptr<Node> copy;
  if (subtitle.empty()) {
    copy = std::move(titleLabel);
  } else {
    copy = ui::column(
        {.align = FlexAlign::Stretch, .gap = 2.0F, .flexGrow = 1.0F}, std::move(titleLabel),
        ui::label(
            {.text = std::move(subtitle),
             .fontSize = Style::fontSizeCaption,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
             .maxLines = 3}
        )
    );
  }
  m_section->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = 8.0F, .paddingV = kRowPaddingV, .minHeight = Style::controlHeight},
          std::move(copy), std::move(control)
      )
  );
  m_rows[key.id()] = [this, key, show = std::move(show)] { show(luciole::preferenceValue(m_current, key)); };
}

template <class T>
void SettingsDialog::addChoice(
    luciole::PreferenceKey<T> key, const std::string& title, const Choices<T>& choices, std::string subtitle
) {
  auto control = std::make_unique<Select>();
  auto* select = control.get();
  std::vector<std::string> options;
  options.reserve(choices.size());
  for (const auto& choice : choices) {
    options.push_back(choice.first);
  }
  select->setOptions(std::move(options));
  select->setMinWidth(180.0F);
  select->setMaxWidth(180.0F);
  select->setFontSize(Style::fontSizeCaption);
  select->setOnSelectionChanged([this, key, choices](std::size_t index, std::string_view) {
    send(key, std::optional<T>(choices[index].second));
  });
  const auto show = [select, choices](const T& value) {
    const auto found = std::ranges::find(choices, value, &Choices<T>::value_type::second);
    select->setSelectedIndexSilently(static_cast<std::size_t>(found - choices.begin()));
  };
  addRow<T>(key, title, show, std::move(control), std::move(subtitle));
}

template <class T> void SettingsDialog::send(luciole::PreferenceKey<T> key, std::optional<T> value) {
  // Choosing the inherited value stores no override.
  if (value && *value == luciole::preferenceValue(m_inherited, key)) {
    value.reset();
  }
  luciole::preferenceValue(m_current, key) = value.value_or(luciole::preferenceValue(m_inherited, key));
  m_rows.at(key.id())();
  setStatus("Saving…", false);
  m_change(value ? luciole::OverrideEdit(key, *value) : luciole::OverrideEdit(key, std::nullopt));
}
