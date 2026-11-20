#include "app/page/preview_edit_controller.h"

#include "app/page/preview_edit_keys.h"
#include "app/page/preview_edit_text.h"
#include "app/page/workbench_session.h"
#include "browser/directory_model.h"
#include "core/deferred_call.h"
#include "fs/location_within.h"
#include "render/scene/node.h"

#include <stdexcept>
#include <string_view>
#include <utility>

namespace luciole {
  namespace {
    constexpr std::string_view kChangedOnDisk =
        "This file changed on disk while it was edited, so saving it is refused.";
  } // namespace

  PreviewEditController::PreviewEditController(Hooks hooks) : m_hooks(std::move(hooks)) {
    if (!m_hooks.frameNeeded
        || !m_hooks.modifiers
        || m_hooks.clipboard == nullptr
        || !m_hooks.focused
        || !m_hooks.focus
        || !m_hooks.focusList
        || !m_hooks.reveal
        || !m_hooks.mayAsk
        || !m_hooks.ask
        || !m_hooks.tell
        || !m_hooks.dismiss) {
      throw std::invalid_argument("A preview editor needs every hook.");
    }
  }

  PreviewEditController::~PreviewEditController() {
    m_probe.cancel();
    if (m_opening) {
      m_opening->cancel();
    }
    m_saveToken.cancel();
    if (m_view != nullptr) {
      dropView();
      m_view->onEnableEditing = {};
      m_view->onSave = {};
      m_view->onStopEditing = {};
      m_view->setKeepsKeyboard(false);
    }
  }

  void PreviewEditController::attach(ColumnPreviewView& view, Slot slot) {
    detach();
    m_view = &view;
    m_slot = std::move(slot);
    view.onEnableEditing = [this] { enable(); };
    view.onSave = [this] { save(); };
    view.onStopEditing = [this] { stop(); };
    // The pane's controls leave the keyboard with the editor while a file is edited.
    view.setKeepsKeyboard(m_session != nullptr);
    refresh();
  }

  void PreviewEditController::refocus() {
    if (auto* area = m_editorNode != nullptr ? m_session->focusArea() : nullptr) {
      m_hooks.focus(*area);
    }
  }

  void PreviewEditController::detach() {
    if (m_view == nullptr) {
      return;
    }
    settle();
    dropView();
    m_view->onEnableEditing = {};
    m_view->onSave = {};
    m_view->onStopEditing = {};
    m_view->setEditBar({});
    m_view->setKeepsKeyboard(false);
    m_view = nullptr;
    m_slot = {};
    offer(nullptr);
    requestLeave();
  }

  bool PreviewEditController::follow(
      const std::optional<Location>& aimed, const DirectoryEntry* entry, bool shown, bool covered
  ) {
    if (m_listed && aimed == m_listed->file()) {
      // A file the listing does not hold now, as under a filter, says nothing about the disk.
      if (entry != nullptr) {
        listed(*entry);
      }
    } else if (!m_session) {
      m_listed.reset(); // another target: the cursor went with it
    }
    if (!m_session) {
      return true;
    }
    if (aimed == m_file && (shown || !covered)) {
      if (shown) {
        showView();
      } else if (textFocused()) {
        m_hooks.focusList(); // keys typed now must not edit text out of sight
      }
      setShown(shown);
      return false;
    }
    setShown(shown);
    requestLeave();
    return !m_session;
  }

  void PreviewEditController::watch(const DirectoryModel& model) {
    if (!m_listed || m_session) {
      return;
    }
    if (model.snapshot().location != m_listed->folder()) {
      m_listed.reset(); // another folder: the file is not this list's
      return;
    }
    // A file the listing does not hold now, as under a filter, says nothing about the disk.
    if (const auto row = rowOf(model, m_listed->file())) {
      listed(*model.entry(*row));
    }
  }

  void PreviewEditController::setShown(bool shown) {
    if (m_editorNode == nullptr || shown == m_editorShown) {
      return;
    }
    m_editorShown = shown;
    m_session->setShown(shown);
    // The page dropped its keyboard focus when it went out of sight; the editor that had it takes it back.
    if (auto* area = shown && m_hadKeyboard ? m_session->focusArea() : nullptr) {
      m_hooks.focus(*area);
    }
  }

  void PreviewEditController::update(float textSize) {
    if (m_editorNode != nullptr && (m_view == nullptr || m_view->content() != m_editorNode)) {
      m_session->dropView(); // the slot let the view go
      m_editorNode = nullptr;
    }
    if (m_editorNode != nullptr && m_editorShown) {
      m_hadKeyboard = textFocused();
      m_session->update(textSize);
    }
  }

  bool PreviewEditController::textFocused() const {
    const auto* area = m_editorNode != nullptr ? m_session->focusArea() : nullptr;
    return area != nullptr && m_hooks.focused(*area);
  }

  bool PreviewEditController::key(const InputArea::KeyData& key) {
    return m_editorNode != nullptr && !leavesEditedFile(key.sym, key.modifiers) && m_session->key(key);
  }

  bool PreviewEditController::unsavedIn(const Location& root) {
    return m_file && locationWithin(*m_file, root) && unsaved();
  }

  bool PreviewEditController::savingIn(const Location& root) const {
    return m_file && locationWithin(*m_file, root) && saving();
  }

  bool PreviewEditController::leave(std::function<void()> resume) {
    if (!m_session) {
      return true;
    }
    requestLeave();
    if (!m_session) {
      return true; // nothing was unsaved: the editor closed at once
    }
    m_resumes.push_back(std::move(resume));
    return false;
  }

  void PreviewEditController::keepQuestion() {
    if (m_question == QuestionState::Shown) {
      m_question = QuestionState::Wanted;
    }
  }

  void PreviewEditController::onFrame() {
    if (m_question != QuestionState::Wanted || !m_gate.asking() || !m_hooks.mayAsk()) {
      return;
    }
    const auto alive = std::weak_ptr<int>(m_alive);
    Question question{
        .name = m_name,
        .sentence = m_questionSentence,
        .save =
            [this, alive] {
              if (!alive.expired()) {
                apply(m_gate.answer(editing::Answer::Save));
              }
            },
        .closed =
            [this, alive] {
              if (!alive.expired()) {
                questionClosed();
              }
            },
    };
    if (m_hooks.ask(std::move(question))) {
      m_question = QuestionState::Shown;
    }
  }

  void PreviewEditController::opened(std::expected<EditableText, std::string> text) {
    if (refusedOpening(text ? nullptr : &text.error())) {
      return;
    }
    const auto identity = text->identity;
    auto notice = text->facts.mixedEndings ? mixedEndingsNotice(text->facts.lineEnding) : std::string();
    begin(std::make_unique<TextEditSession>(std::move(*text), m_offer->entry.name), identity, std::move(notice));
  }

  bool PreviewEditController::refusedOpening(const std::string* reason) {
    m_opening.reset();
    if (!m_offer) {
      return true;
    }
    if (reason != nullptr) {
      m_offer->reason = *reason;
      refresh();
      m_hooks.frameNeeded();
      return true;
    }
    return m_gate.enable(true).refusal != editing::Refusal::None;
  }

  void PreviewEditController::begin(
      std::unique_ptr<PreviewEditSession> session, const UserFileIdentity& identity, std::string notice
  ) {
    const auto entry = std::move(m_offer->entry);
    m_offer.reset();
    m_file = entry.location();
    m_name = entry.displayName();
    m_identity = identity;
    m_listed.emplace(entry);
    m_savedShape.reset();
    m_leaving = false;
    m_notice = std::move(notice);
    m_session = std::move(session);
    if (m_view != nullptr) {
      m_view->setKeepsKeyboard(true);
    }
    showView();
    refocus();
    refresh();
    m_hooks.frameNeeded();
  }

  void PreviewEditController::save() {
    if (m_session) {
      settle();
      apply(m_gate.save());
    }
  }

  void PreviewEditController::stop() {
    if (m_session) {
      settle();
      apply(m_gate.stop());
    }
  }

  void PreviewEditController::saved(UserFileSaveResult result) {
    if (!m_session) {
      return;
    }
    if (result.status != UserFileSaveStatus::Saved && result.status != UserFileSaveStatus::SavedNotFlushed) {
      if (m_listed) {
        m_listed->saveRefused();
      }
      apply(m_gate.saveFinished(std::unexpected(saveSentence(result))));
      return;
    }
    if (result.identity) {
      m_identity = *result.identity;
      if (m_listed) {
        m_listed->saved(*result.identity);
      }
    }
    m_session->saved(m_saveRevision);
    m_savedShape = m_savingShape;
    m_notice = saveSentence(result);
    apply(m_gate.saveFinished(editing::SaveSucceeded{.changedSinceStart = m_session->modified()}));
  }

  void PreviewEditController::questionClosed() {
    if (m_question != QuestionState::Shown) {
      return;
    }
    m_question = QuestionState::None;
    const auto result = m_gate.answer(editing::Answer::No);
    if (result.refusal == editing::Refusal::SaveRunning) {
      m_question = QuestionState::Wanted; // closed while its save runs: no answer, it comes back
      return;
    }
    apply(result);
  }

  void PreviewEditController::requestLeave() {
    if (m_session && !m_leaving) {
      settle();
      m_leaving = true;
      apply(m_gate.leave());
    }
  }

  void PreviewEditController::apply(const editing::GateResult& result) {
    if (result.refusal != editing::Refusal::None) {
      return;
    }
    if (result.closeQuestion) {
      closeQuestion();
    }
    if (result.openQuestion) {
      m_question = QuestionState::Wanted;
      m_questionSentence.clear();
    }
    if (result.notice == editing::Notice::SaveFailed) {
      failed(result.sentence);
    } else if (result.notice == editing::Notice::ChangedOnDisk) {
      m_notice = std::string(kChangedOnDisk);
    }
    if (result.startSave) {
      m_saveRevision = m_session->revision();
      m_listed.value().saveStarted();
      m_saveToken = CancelToken{};
      auto bytes = m_session->fileBytes();
      m_savingShape = m_session->shape(bytes);
      startUserFileSave(
          *m_file, std::move(bytes), m_identity, m_saveToken,
          [this, alive = std::weak_ptr<int>(m_alive)](UserFileSaveResult saveResult) {
            if (!alive.expired()) {
              saved(std::move(saveResult));
            }
          }
      );
    }
    if (result.closeEditor) {
      close();
    }
    if (result.proceed != editing::Proceed::None) {
      // Never from inside the popup's own close: the page goes on from the main loop.
      for (auto& resume : std::exchange(m_resumes, {})) {
        DeferredCall::callLater(std::move(resume));
      }
    }
    refresh();
    if (m_question == QuestionState::Wanted) {
      m_hooks.frameNeeded(); // onFrame() shows it, also when the view is gone and nothing else draws
    }
  }

  void PreviewEditController::failed(const std::string& sentence) {
    if (!m_gate.asking()) {
      m_notice = sentence;
      return;
    }
    m_questionSentence = sentence;
    if (m_question == QuestionState::Shown) {
      m_hooks.tell(sentence);
    }
  }

  void PreviewEditController::closeQuestion() {
    m_questionSentence.clear();
    if (std::exchange(m_question, QuestionState::None) == QuestionState::Shown) {
      m_hooks.dismiss();
    }
  }

  void PreviewEditController::close() {
    const bool keyboard = m_editorNode != nullptr && m_editorShown && (m_hadKeyboard || textFocused());
    dropView();
    m_table = nullptr;
    m_session.reset();
    m_savedShape.reset();
    if (m_view != nullptr) {
      m_view->setKeepsKeyboard(false);
    }
    m_file.reset();
    m_notice.clear();
    if (m_listed && !m_listed->awaited()) {
      m_listed.reset();
    }
    m_leaving = false;
    refresh();
    if (m_slot.closed) {
      m_slot.closed();
    }
    if (keyboard) {
      m_hooks.focusList(); // its input area went with the editor
    }
  }

  void PreviewEditController::showView() {
    if (m_view == nullptr || !m_session || m_editorNode != nullptr) {
      return;
    }
    m_slot.release();
    auto view = m_session->makeView(
        *m_hooks.clipboard,
        PreviewEditEvents{
            .edited = [this] { apply(m_gate.setDirty(m_session->modified())); },
            .saveRequested = [this] { save(); },
            .escape = [this] { stop(); },
            .refused =
                [this](const std::string& sentence) {
                  m_notice = sentence;
                  refresh();
                },
            .frameNeeded = [this] { m_hooks.frameNeeded(); },
            .modifiers = [this] { return m_hooks.modifiers(); },
        }
    );
    m_editorNode = view.get();
    m_editorShown = true;
    m_hadKeyboard = false;
    m_view->setContent(std::move(view));
  }

  void PreviewEditController::dropView() {
    if (m_editorNode == nullptr) {
      return;
    }
    m_session->dropView();
    if (m_view != nullptr && m_view->content() == m_editorNode) {
      m_view->setContent(nullptr);
    }
    m_editorNode = nullptr;
  }

  void PreviewEditController::listed(const DirectoryEntry& entry) {
    const auto seen = m_listed->see(entry, m_identity, m_gate.saving());
    if (seen.relisted) {
      m_hooks.reveal(m_listed->file());
    }
    if (seen.changed && m_session) {
      apply(m_gate.changedOnDisk());
    }
    if (!m_session && !m_listed->awaited()) {
      m_listed.reset();
    }
  }

  void PreviewEditController::refresh() {
    if (m_view != nullptr) {
      m_view->setEditBar(bar());
    }
  }

  ColumnPreviewEditBar PreviewEditController::bar() const {
    if (m_session) {
      return {
          .shown = true,
          .editing = true,
          .enableAvailable = false,
          .reason = {},
          .saveAvailable = m_gate.saveEnabled(),
          .unsaved = m_gate.dirty(),
          .notice = m_notice,
      };
    }
    if (!m_offer) {
      return {};
    }
    return {
        .shown = true,
        .editing = false,
        .enableAvailable = m_offer->checked && m_offer->reason.empty() && !m_opening,
        .reason = m_offer->reason,
        .saveAvailable = false,
        .unsaved = false,
        .notice = {},
    };
  }
} // namespace luciole
