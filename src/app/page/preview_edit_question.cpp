#include "app/page/preview_edit_question.h"

#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/dialogs/dialog_chrome.h"
#include "ui/style.h"

#include <format>
#include <string>
#include <utility>

namespace luciole {
  namespace {
    constexpr int kSentenceLines = 3;
  } // namespace

  SaveQuestion saveQuestion(std::string_view name, std::function<void()> save, std::function<void()> no) {
    SaveQuestion question;
    auto close = no;
    question.root = ui::column(
        {.align = FlexAlign::Stretch},
        dialogHeader(std::format("Save changes to {}?", name), "device-floppy", std::move(close)),
        ui::label(
            {.out = &question.sentence,
             .color = colorSpecFromRole(ColorRole::Error),
             .maxLines = kSentenceLines,
             .configure = [](Label& label) { label.setUseMarkup(false); }}
        ),
        ui::row(
            {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
            ui::button({.out = &question.no, .text = "No", .onClick = std::move(no)}),
            ui::button(
                {.out = &question.save, .text = "Save", .variant = ButtonVariant::Primary, .onClick = std::move(save)}
            )
        )
    );
    applyDialogStyle(*question.root);
    showQuestionSentence(question, {});
    return question;
  }

  void showQuestionSentence(const SaveQuestion& question, std::string_view sentence) {
    question.sentence->setText(sentence);
    question.sentence->setVisible(!sentence.empty());
    question.sentence->setParticipatesInLayout(!sentence.empty());
  }
} // namespace luciole
