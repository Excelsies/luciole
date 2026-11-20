#include "editing/text_buffer.h"
#include "test_check.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <print>
#include <string>
#include <string_view>

namespace {
  using namespace luciole::editing;

  TextBuffer make(std::string_view text, TextSelection selection = {}) {
    auto buffer = TextBuffer::create(text);
    TEST_CHECK(buffer.has_value());
    buffer->setSelection(selection);
    return std::move(*buffer);
  }

  TextSelection caret(std::size_t line, std::size_t byte) { return {{line, byte}, {line, byte}}; }

  void changed(const EditResult& result) { TEST_CHECK(result.has_value() && result->has_value()); }

  // Runs `edit` on a buffer, then checks that undo restores the text and selection and redo restores the edit.
  void checkUndoRedo(
      std::string_view text, TextSelection selection, const std::function<EditResult(TextBuffer&)>& edit,
      LineSpan undoSpan
  ) {
    auto buffer = make(text, selection);
    changed(edit(buffer));
    const auto editedText = buffer.text();
    const auto editedSelection = buffer.selection();
    TEST_CHECK(buffer.revision() == 1);
    const auto undone = buffer.undo();
    TEST_CHECK(undone.has_value() && *undone == undoSpan);
    TEST_CHECK(buffer.text() == text && buffer.selection() == selection && buffer.revision() == 2);
    const auto redone = buffer.redo();
    TEST_CHECK(redone.has_value() && redone->firstLine == undoSpan.firstLine);
    TEST_CHECK(redone->removedLines == undoSpan.insertedLines && redone->insertedLines == undoSpan.removedLines);
    TEST_CHECK(buffer.text() == editedText && buffer.selection() == editedSelection && buffer.revision() == 3);
    TEST_CHECK(!buffer.redo().has_value() && buffer.revision() == 3);
  }

  void testUndoEachEdit() {
    const IndentUnit two{.spaces = 2};
    checkUndoRedo("ab", caret(0, 1), [](TextBuffer& b) { return b.insertText("X", 0); }, {0, 1, 1});
    checkUndoRedo("one\ntwo\nthree", {{2, 2}, {0, 1}}, [](TextBuffer& b) { return b.insertText("Z", 0); }, {0, 1, 3});
    checkUndoRedo("ab", caret(0, 1), [](TextBuffer& b) { return b.insertText("p\r\nq\n", 0); }, {0, 3, 1});
    checkUndoRedo("  ab", caret(0, 3), [](TextBuffer& b) { return b.newline(0); }, {0, 2, 1});
    checkUndoRedo("a\nb", caret(1, 0), [](TextBuffer& b) { return b.eraseBackward(EraseUnit::Cluster, 0); }, {0, 1, 2});
    checkUndoRedo("foo bar", caret(0, 7), [](TextBuffer& b) { return b.eraseBackward(EraseUnit::Word, 0); }, {0, 1, 1});
    checkUndoRedo(
        "foo\nbar", caret(1, 2), [](TextBuffer& b) { return b.eraseBackward(EraseUnit::LineEdge, 0); }, {1, 1, 1}
    );
    checkUndoRedo("a\nb", caret(0, 1), [](TextBuffer& b) { return b.eraseForward(EraseUnit::Cluster, 0); }, {0, 1, 2});
    checkUndoRedo("foo bar", caret(0, 0), [](TextBuffer& b) { return b.eraseForward(EraseUnit::Word, 0); }, {0, 1, 1});
    checkUndoRedo(
        "foo\nbar", caret(0, 1), [](TextBuffer& b) { return b.eraseForward(EraseUnit::LineEdge, 0); }, {0, 1, 1}
    );
    checkUndoRedo(
        "a\nb\nc", {{0, 0}, {2, 1}}, [](TextBuffer& b) { return b.eraseForward(EraseUnit::Word, 0); }, {0, 1, 3}
    );
    checkUndoRedo("a\nb", {{1, 1}, {0, 1}}, [&](TextBuffer& b) { return b.indent(two, 0); }, {0, 2, 2});
    checkUndoRedo("ab", caret(0, 1), [&](TextBuffer& b) { return b.indent(two, 0); }, {0, 1, 1});
    checkUndoRedo("  a\n  b", {{0, 3}, {1, 3}}, [&](TextBuffer& b) { return b.outdent(two, 0); }, {0, 2, 2});
    checkUndoRedo("a  b", caret(0, 3), [&](TextBuffer& b) { return b.outdent(two, 0); }, {0, 1, 1});
  }

  void testEmptyLog() {
    auto buffer = make("abc");
    TEST_CHECK(!buffer.undo().has_value() && !buffer.redo().has_value());
    TEST_CHECK(buffer.revision() == 0 && !buffer.modified());
    // A refused or empty edit records nothing.
    TEST_CHECK(!buffer.insertText("\xFF", 0).has_value());
    TEST_CHECK(buffer.eraseBackward(EraseUnit::Cluster, 0).has_value());
    TEST_CHECK(!buffer.undo().has_value() && buffer.revision() == 0);
  }

  // Types each code point of `text` as its own insert, `gapMs` apart from `startMs` on.
  void type(TextBuffer& buffer, std::string_view text, std::uint64_t startMs, std::uint64_t gapMs) {
    for (std::size_t i = 0; i < text.size(); ++i) {
      changed(buffer.insertText(text.substr(i, 1), startMs + (i * gapMs)));
    }
  }

  // How many undo steps the buffer holds; leaves it at the oldest state.
  std::size_t undoAll(TextBuffer& buffer) {
    std::size_t steps = 0;
    while (buffer.undo().has_value()) {
      ++steps;
    }
    return steps;
  }

  void testTypingGroups() {
    auto joined = make("");
    type(joined, "abc", 0, 500);
    TEST_CHECK(joined.revision() == 3);
    TEST_CHECK(undoAll(joined) == 1 && joined.text().empty() && (joined.selection() == caret(0, 0)));

    auto slow = make("");
    type(slow, "ab", 0, 1001);
    TEST_CHECK(undoAll(slow) == 2);

    auto exact = make("");
    type(exact, "ab", 0, 1000);
    TEST_CHECK(undoAll(exact) == 1);

    auto backwards = make("");
    type(backwards, "a", 5000, 0);
    type(backwards, "b", 4000, 0);
    TEST_CHECK(undoAll(backwards) == 2);

    auto moved = make("");
    type(moved, "ab", 0, 10);
    moved.setSelection(caret(0, 0));
    moved.setSelection(caret(0, 2));
    type(moved, "c", 30, 0);
    TEST_CHECK(undoAll(moved) == 2);

    auto stayed = make("");
    type(stayed, "ab", 0, 10);
    stayed.setSelection(caret(0, 2));
    type(stayed, "c", 30, 0);
    TEST_CHECK(undoAll(stayed) == 1);

    // A ZWJ sequence is one cluster, so it types like a letter.
    auto emoji = make("");
    type(emoji, "a", 0, 0);
    changed(emoji.insertText("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9", 10));
    TEST_CHECK(undoAll(emoji) == 1);
  }

  void testWhatBreaksTyping() {
    auto paste = make("");
    type(paste, "a", 0, 0);
    changed(paste.insertText("xy", 10));
    type(paste, "b", 20, 0);
    TEST_CHECK(paste.text() == "axyb" && undoAll(paste) == 3);

    auto newline = make("");
    type(newline, "a", 0, 0);
    changed(newline.newline(10));
    type(newline, "b", 20, 0);
    TEST_CHECK(undoAll(newline) == 3);

    auto erase = make("");
    type(erase, "ab", 0, 10);
    changed(erase.eraseBackward(EraseUnit::Cluster, 30));
    type(erase, "c", 40, 0);
    TEST_CHECK(erase.text() == "ac" && undoAll(erase) == 3);

    auto replaced = make("xy", {{0, 0}, {0, 1}});
    type(replaced, "a", 0, 0);
    type(replaced, "b", 10, 0);
    TEST_CHECK(replaced.text() == "aby" && undoAll(replaced) == 2);

    auto undone = make("");
    type(undone, "ab", 0, 10);
    TEST_CHECK(undone.undo().has_value() && undone.redo().has_value());
    type(undone, "c", 30, 0);
    TEST_CHECK(undoAll(undone) == 2);
  }

  void testEraseGroups() {
    auto backward = make("abcd", caret(0, 4));
    changed(backward.eraseBackward(EraseUnit::Cluster, 0));
    changed(backward.eraseBackward(EraseUnit::Cluster, 400));
    changed(backward.eraseBackward(EraseUnit::Word, 800));
    TEST_CHECK(backward.text().empty() && backward.revision() == 3);
    TEST_CHECK(undoAll(backward) == 1 && backward.text() == "abcd" && (backward.selection() == caret(0, 4)));
    TEST_CHECK(backward.redo().has_value() && backward.text().empty() && (backward.selection() == caret(0, 0)));

    auto forward = make("ab\ncd", caret(0, 1));
    changed(forward.eraseForward(EraseUnit::Cluster, 0));
    changed(forward.eraseForward(EraseUnit::Cluster, 10));
    changed(forward.eraseForward(EraseUnit::Cluster, 20));
    TEST_CHECK(forward.text() == "ad" && undoAll(forward) == 1 && forward.text() == "ab\ncd");
    TEST_CHECK((forward.selection() == caret(0, 1)));

    auto mixed = make("abcd", caret(0, 2));
    changed(mixed.eraseBackward(EraseUnit::Cluster, 0));
    changed(mixed.eraseForward(EraseUnit::Cluster, 10));
    changed(mixed.eraseBackward(EraseUnit::Cluster, 20));
    TEST_CHECK(mixed.text() == "d" && undoAll(mixed) == 3);

    auto selection = make("abcd", caret(0, 4));
    changed(selection.eraseBackward(EraseUnit::Cluster, 0));
    selection.setSelection({{0, 1}, {0, 2}});
    changed(selection.eraseBackward(EraseUnit::Cluster, 10));
    changed(selection.eraseBackward(EraseUnit::Cluster, 20));
    TEST_CHECK(selection.text() == "c" && undoAll(selection) == 3);
  }

  void testStepBound() {
    auto buffer = make("");
    type(buffer, std::string(1001, 'x'), 0, 2000);
    TEST_CHECK(buffer.text().size() == 1001);
    TEST_CHECK(undoAll(buffer) == 1000 && buffer.text() == "x");
    // The saved state, the empty text, was dropped.
    TEST_CHECK(buffer.modified());
  }

  // 1,000 lines of 999 bytes, each ending in LF.
  std::string megabyteBlock() {
    std::string block;
    for (int line = 0; line < 1000; ++line) {
      block += std::string(999, 'b') + '\n';
    }
    return block;
  }

  void testByteBound() {
    const auto block = megabyteBlock();
    TEST_CHECK(block.size() == 1'000'000);
    auto buffer = make("");
    for (std::uint64_t round = 0; round < 5; ++round) {
      changed(buffer.insertText(block, round * 10));
      buffer.setSelection({{0, 0}, {1000, 0}});
      changed(buffer.eraseBackward(EraseUnit::Cluster, (round * 10) + 5));
    }
    // Ten steps store 1,000,000 bytes each; eight fit in 8 MiB.
    TEST_CHECK(undoAll(buffer) == 8 && buffer.text().empty());
  }

  void testByteBoundAfterDiscardedRedo() {
    const auto block = megabyteBlock();
    auto buffer = make("");
    // Each new edit discards the undone one, and its bytes with it.
    for (std::uint64_t round = 0; round < 9; ++round) {
      changed(buffer.insertText(block, round * 10));
      TEST_CHECK(buffer.undo().has_value());
    }
    for (std::uint64_t round = 0; round < 4; ++round) {
      changed(buffer.insertText(block, 100 + (round * 10)));
      if (round < 3) {
        buffer.setSelection({{0, 0}, {1000, 0}});
        changed(buffer.eraseBackward(EraseUnit::Cluster, 105 + (round * 10)));
      }
    }
    // Seven steps of 1,000,000 bytes fit in 8 MiB.
    TEST_CHECK(undoAll(buffer) == 7 && buffer.text().empty());
  }

  void testByteBoundOfJoinedErases() {
    const auto block = megabyteBlock();
    auto buffer = make("");
    std::uint64_t nowMs = 0;
    for (int round = 0; round < 5; ++round) {
      changed(buffer.insertText(block, nowMs));
      // 2,000 erases 1 ms apart join one step: each line's 999 bytes, then the LF before it.
      for (int erase = 0; erase < 2000; ++erase) {
        changed(buffer.eraseBackward(EraseUnit::LineEdge, ++nowMs));
      }
      TEST_CHECK(buffer.text().empty());
      nowMs += 10'000;
    }
    // Five pastes and five erase groups store 1,000,000 bytes each; eight fit in 8 MiB.
    TEST_CHECK(undoAll(buffer) == 8 && buffer.text().empty());
  }

  void testModifiedThroughSave() {
    auto buffer = make("");
    TEST_CHECK(!buffer.modified());
    type(buffer, "a", 0, 0);
    TEST_CHECK(buffer.modified());
    TEST_CHECK(buffer.undo().has_value() && !buffer.modified());
    TEST_CHECK(buffer.redo().has_value() && buffer.modified());
    TEST_CHECK(buffer.markSaved(buffer.revision()) && !buffer.modified());
    TEST_CHECK(buffer.undo().has_value() && buffer.modified());
    TEST_CHECK(buffer.redo().has_value() && !buffer.modified());
    type(buffer, "b", 5000, 0);
    TEST_CHECK(buffer.modified());
    TEST_CHECK(buffer.undo().has_value() && buffer.text() == "a" && !buffer.modified());

    // A save closes the typing group, so undo can come back to the saved text.
    auto typing = make("");
    type(typing, "a", 0, 0);
    TEST_CHECK(typing.markSaved(typing.revision()));
    type(typing, "b", 100, 0);
    TEST_CHECK(typing.modified() && typing.undo().has_value() && typing.text() == "a" && !typing.modified());
  }

  void testSaveFinishedAfterMoreTyping() {
    // Typing that joined the written step changed it: the written text cannot come back.
    auto joined = make("");
    type(joined, "abc", 0, 100);
    const auto written = joined.revision();
    type(joined, "d", 400, 0);
    TEST_CHECK(!joined.markSaved(written) && joined.modified());
    TEST_CHECK(joined.undo().has_value() && joined.text().empty() && joined.modified());
    TEST_CHECK(joined.markSaved(joined.revision()) && !joined.modified());

    // Typing in a new step leaves the written state reachable.
    auto separate = make("");
    type(separate, "abc", 0, 100);
    const auto saved = separate.revision();
    type(separate, "d", 5000, 0);
    TEST_CHECK(separate.markSaved(saved) && separate.modified());
    TEST_CHECK(separate.undo().has_value() && separate.text() == "abc" && !separate.modified());

    // The written state was undone and replaced by another edit.
    auto overwritten = make("");
    type(overwritten, "a", 0, 0);
    const auto first = overwritten.revision();
    TEST_CHECK(overwritten.undo().has_value());
    type(overwritten, "b", 5000, 0);
    TEST_CHECK(!overwritten.markSaved(first) && overwritten.modified());
    TEST_CHECK(overwritten.undo().has_value() && overwritten.modified());

    // The written state came back through undo before the save finished.
    auto returned = make("");
    type(returned, "a", 0, 0);
    const auto again = returned.revision();
    type(returned, "b", 5000, 0);
    TEST_CHECK(returned.undo().has_value());
    TEST_CHECK(returned.markSaved(again) && !returned.modified());
  }

  void testRevisions() {
    auto buffer = make("ab", caret(0, 2));
    type(buffer, "x", 0, 0);
    type(buffer, "y", 10, 0);
    TEST_CHECK(buffer.revision() == 2);
    buffer.setSelection(caret(0, 0));
    TEST_CHECK(!buffer.insertText(std::string_view("\0", 1), 20).has_value());
    TEST_CHECK(buffer.eraseBackward(EraseUnit::Cluster, 30).has_value());
    TEST_CHECK(buffer.revision() == 2);
    TEST_CHECK(buffer.undo().has_value() && buffer.revision() == 3);
    TEST_CHECK(buffer.redo().has_value() && buffer.revision() == 4);
    TEST_CHECK(!buffer.markSaved(5) && buffer.modified());
  }

  void testRevisionMemory() {
    auto buffer = make("");
    type(buffer, "a", 0, 0);
    for (int pair = 0; pair < 2049; ++pair) {
      TEST_CHECK(buffer.undo().has_value() && buffer.redo().has_value());
    }
    TEST_CHECK(buffer.revision() == 4099 && buffer.text() == "a");
    TEST_CHECK(!buffer.markSaved(1) && !buffer.markSaved(buffer.revision() - 4096));
    // An even revision is an undo: the empty text.
    TEST_CHECK(buffer.markSaved(buffer.revision() - 4095) && buffer.modified());
    TEST_CHECK(buffer.undo().has_value() && !buffer.modified());
  }
} // namespace

int main() {
  testUndoEachEdit();
  testEmptyLog();
  testTypingGroups();
  testWhatBreaksTyping();
  testEraseGroups();
  testStepBound();
  testByteBound();
  testByteBoundAfterDiscardedRedo();
  testByteBoundOfJoinedErases();
  testModifiedThroughSave();
  testSaveFinishedAfterMoreTyping();
  testRevisions();
  testRevisionMemory();
  std::println("editing_text_history_test: ok");
  return 0;
}
