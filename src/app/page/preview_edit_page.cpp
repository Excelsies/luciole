#include "app/page/preview_edit_page.h"

#include "app/page/active_popup.h"
#include "app/page/preview_edit_question.h"
#include "app/page/workbench_session.h"
#include "clipboard/text_clipboard.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_seat.h"

#include <string>
#include <utility>

std::unique_ptr<luciole::PreviewEditController>
pagePreviewEditor(PageContext& page, std::function<std::uint32_t()> modifiers) {
  // The parts of the question while it is open; the dialog owns them.
  auto shown = std::make_shared<luciole::SaveQuestion>();
  return std::make_unique<luciole::PreviewEditController>(luciole::PreviewEditController::Hooks{
      .frameNeeded = [&page] { page.surface().requestUpdate(); },
      .modifiers = std::move(modifiers),
      .clipboard = &page.clipboard(),
      .focused = [&page](const InputArea& area) { return page.dispatcher().focusedArea() == &area; },
      .focus =
          [&page](InputArea& area) {
            if (page.active()) {
              page.dispatcher().setFocus(&area);
            }
          },
      .focusList =
          [&page] {
            if (page.active()) {
              page.focusFileList();
            }
          },
      .reveal =
          [&page](const luciole::Location& file) {
            if (luciole::restoreCursor(page.browser().model(), file)) {
              page.requestSync();
            }
          },
      .mayAsk = [&page] { return page.active() && page.popupAvailable() && page.popupSurface() == nullptr; },
      .ask =
          [&page, shown](luciole::PreviewEditController::Question question) {
            // No and the header's close button close the dialog; its close, Escape included, is an answer only when
            // No was pressed first.
            auto parts = luciole::saveQuestion(
                question.name, question.save,
                [&page, no = question.no] {
                  no();
                  page.dismissPopup();
                },
                [&page] { page.dismissPopup(); }
            );
            luciole::showQuestionSentence(parts, question.sentence);
            page.showDialog(std::move(parts.root), DialogWidth::Small);
            if (page.popupSurface() == nullptr) {
              return false;
            }
            *shown = std::move(parts);
            page.activePopup().bind(ActivePopup::Kind::SaveChanges, [shown, closed = std::move(question.closed)] {
              *shown = {};
              closed();
            });
            page.popupDispatcher().setFocus(shown->save->inputArea());
            return true;
          },
      .tell =
          [&page, shown](const std::string& sentence) {
            if (shown->sentence != nullptr && page.activePopup().kind() == ActivePopup::Kind::SaveChanges) {
              luciole::showQuestionSentence(*shown, sentence);
              page.popupSurface()->requestLayout();
            }
          },
      .dismiss =
          [&page] {
            if (page.activePopup().kind() == ActivePopup::Kind::SaveChanges) {
              page.dismissPopup();
            }
          },
  });
}

PageEditing pageEditing(luciole::PreviewEditController& editor) {
  return {
      .textFocused = [&editor] { return editor.textFocused(); },
      .key =
          [&editor](const KeyboardEvent& event) {
            return editor.key(
                {.sym = event.sym,
                 .utf32 = event.utf32,
                 .modifiers = event.modifiers,
                 .pressed = event.pressed,
                 .preedit = event.preedit}
            );
          },
      .unsaved = [&editor] { return editor.unsaved(); },
      .saving = [&editor] { return editor.saving(); },
      .leave = [&editor](
                   std::function<void()> resume, bool dismissCancels
               ) { return editor.leave(std::move(resume), dismissCancels); },
      .keepQuestion = [&editor] { editor.keepQuestion(); },
  };
}
