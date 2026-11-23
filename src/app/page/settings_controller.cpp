#include "app/page/settings_controller.h"

#include "app/page/active_popup.h"
#include "app/page/dialog_dropdown.h"
#include "app/page/page_context.h"
#include "config/view_preference_writer.h"
#include "config/visit_service.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/browser/action_shortcuts.h"
#include "ui/dialogs/settings_dialog.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_seat.h"

#include <utility>

SettingsController::SettingsController(
    PageContext& page, luciole::ViewPreferenceWriter& preferences, luciole::ThumbnailService& thumbnails,
    luciole::VisitService& visits, std::function<void()> clearThumbnails
)
    : m_page(page), m_preferences(preferences), m_thumbnails(thumbnails), m_visits(visits),
      m_clearThumbnails(std::move(clearThumbnails)) {}

void SettingsController::visitsChanged() {
  if (!m_clearingVisits || m_visits.busy()) {
    return;
  }
  m_clearingVisits = false;
  m_visitWarning = m_visits.warning();
  m_dirty = true;
}

void SettingsController::clearVisits() {
  m_clearingVisits = true;
  m_visitWarning.clear();
  m_dirty = true;
  m_visits.clear();
}

void SettingsController::preferencesChanged(const std::string& error) {
  m_error = error;
  m_pending = m_preferences.settings();
  m_dirty = true;
}

void SettingsController::onFrame() {
  if (std::exchange(m_dirty, false)) {
    if (m_pending) {
      m_page.applySettings(m_pending->browser, !m_error.empty());
    }
    if (m_view) {
      sync();
      if (auto* surface = m_page.popupSurface()) {
        surface->requestLayout();
      }
    }
  }
}

void SettingsController::layoutDialog(float width) {
  if (m_view) {
    m_view->setCompact(width < 720.0F);
  }
}

void SettingsController::sync() {
  const auto& inherited = m_preferences.inheritedSettings();
  const auto& error = !m_error.empty() ? m_error
      : !m_thumbnails.error().empty()  ? m_thumbnails.error()
                                       : m_visitWarning;
  m_view->sync(
      m_page.settings(), inherited ? inherited->browser : luciole::BrowserSettings{},
      m_preferences.busy() || m_thumbnails.clearing(), error, m_thumbnails.clearing(), m_clearingVisits
  );
}

void SettingsController::teardown() {
  m_view = nullptr;
  m_page.focusFileList();
}

void SettingsController::toggle() {
  if (m_view) {
    m_page.dismissPopup();
    return;
  }
  m_visitWarning.clear();
  auto root = std::make_unique<SettingsDialog>(
      [this](luciole::OverrideEdit edit) {
        m_preferences.submit({std::move(edit)});
        if (auto* surface = m_page.popupSurface()) {
          surface->requestLayout();
        }
      },
      [this] { m_preferences.resetAll(); }, [this] { m_page.dismissPopup(); }, [this] { m_clearThumbnails(); },
      [this] { clearVisits(); }, m_page.surface().supportsSSD()
  );
  root->setPopupContext(&m_page.dialogDropdown());
  m_view = root.get();
  sync();
  m_view->setCompact(m_page.dialogWidth(DialogWidth::Large) < 720.0F);
  m_page.showDialog(std::move(root), DialogWidth::Large);
  if (!m_page.popupSurface()) {
    m_view = nullptr;
    return;
  }
  m_page.activePopup().bind(ActivePopup::Kind::Settings, [this] { teardown(); });
  m_page.popupDispatcher().setFocusChangeCallback([this](InputArea*, InputArea* focused) {
    if (m_view) {
      m_view->reveal(focused);
      m_page.popupSurface()->requestLayout();
    }
  });
  m_page.popupDispatcher().setFocus(m_page.popupDispatcher().firstTabFocusUnder(m_view->navigation()));
}

bool SettingsController::key(const KeyboardEvent& event) {
  if (!m_view) {
    return false;
  }
  // While a keybind recorder is capturing, its input area owns the keys, including Cancel and Tab.
  if (auto* focused = m_page.popupDispatcher().focusedArea(); focused && focused->capturesAllKeys()) {
    focused->dispatchKey(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    return true;
  }
  if (event.pressed && luciole::shortcutMatches(luciole::BrowserAction::Settings, event.sym, event.modifiers)) {
    m_page.dismissPopup();
    return true;
  }
  return false;
}
