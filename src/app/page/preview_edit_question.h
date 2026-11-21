#pragma once

#include <functional>
#include <memory>
#include <string_view>

class Button;
class Flex;
class Label;

namespace luciole {
  // The "Save changes to <name>?" question as a dialog root, with the parts kept while it is open.
  struct SaveQuestion {
    std::unique_ptr<Flex> root;
    Label* sentence = nullptr; // why a save failed; hidden while empty
    Button* save = nullptr;
    Button* no = nullptr;
  };

  // The header with the question and its close button, a line for a failed save's sentence, then No and Save. Save
  // calls `save`, No calls `no` and the close button calls `close`.
  SaveQuestion saveQuestion(
      std::string_view name, std::function<void()> save, std::function<void()> no, std::function<void()> close
  );

  // Shows `sentence` under the question, or hides the line when it is empty.
  void showQuestionSentence(const SaveQuestion& question, std::string_view sentence);
} // namespace luciole
