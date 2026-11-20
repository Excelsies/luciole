#include "app/page/preview_edit_text.h"
#include "editing/text_buffer.h"
#include "formats/text_facts.h"
#include "fs/user_file_save.h"
#include "temp_folder.h"
#include "test_check.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <optional>
#include <print>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

// A file read whole for editing in a column preview, through real files in a private folder.
namespace {
  namespace fs = std::filesystem;
  using luciole::CancelToken;
  using luciole::Location;
  using luciole::formats::LineEnding;
  using Status = luciole::UserFileSaveStatus;

  struct Fixture {
    luciole::test::TempFolder tempFolder{"luciole-edit-text-test"};
    fs::path directory = tempFolder.path();

    fs::path write(const std::string& name, const std::string& bytes, mode_t mode = 0644) const {
      const auto path = directory / name;
      TEST_CHECK((std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes).good());
      TEST_CHECK(::chmod(path.c_str(), mode) == 0);
      return path;
    }
  };

  Location at(const fs::path& path) { return Location::fromPath(path.native()); }

  std::expected<luciole::EditableText, std::string> read(const fs::path& path) {
    return luciole::readEditableText(at(path), {});
  }

  std::string refusal(const fs::path& path) {
    const auto text = read(path);
    TEST_CHECK(!text.has_value());
    return text ? std::string() : text.error();
  }

  // The bytes come back unchanged through the editor's text: the line endings, a byte order mark and a missing
  // final newline included.
  void testWholeTextKeepsItsBytes() {
    const Fixture fixture;
    struct Case {
      const char* name;
      std::string bytes;
      std::string text;
      LineEnding ending;
      bool mark;
      bool finalNewline;
    };
    const Case cases[] = {
        {"lf.txt", "one\ntwo\n", "one\ntwo\n", LineEnding::Lf, false, true},
        {"crlf.txt", "one\r\ntwo\r\n", "one\ntwo\n", LineEnding::CrLf, false, true},
        {"mark.txt", "\xEF\xBB\xBFone\ntwo\n", "one\ntwo\n", LineEnding::Lf, true, true},
        {"open.txt", "one\ntwo", "one\ntwo", LineEnding::Lf, false, false},
        {"empty.txt", "", "", LineEnding::None, false, false},
    };
    for (const auto& item : cases) {
      const auto path = fixture.write(item.name, item.bytes);
      const auto text = read(path);
      TEST_CHECK(text.has_value());
      if (!text) {
        continue;
      }
      TEST_CHECK(text->buffer.text() == item.text);
      TEST_CHECK(text->facts.lineEnding == item.ending);
      TEST_CHECK(text->facts.byteOrderMark == item.mark && text->facts.finalNewline == item.finalNewline);
      TEST_CHECK(!text->facts.mixedEndings);
      TEST_CHECK(luciole::formats::fileBytes(text->buffer.text(), text->facts) == item.bytes);
      struct stat info{};
      TEST_CHECK(::stat(path.c_str(), &info) == 0);
      TEST_CHECK(text->identity.inode == info.st_ino && text->identity.size == item.bytes.size());
      TEST_CHECK(!text->buffer.modified());
    }
  }

  void testMixedEndingsAreNamed() {
    const Fixture fixture;
    const auto text = read(fixture.write("mixed.txt", "a\r\nb\r\nc\nd\r\n"));
    TEST_CHECK(text && text->facts.mixedEndings && text->facts.lineEnding == LineEnding::CrLf);
    const auto notice = luciole::mixedEndingsNotice(LineEnding::CrLf);
    TEST_CHECK(notice.contains("CRLF") && notice.ends_with('.'));
    TEST_CHECK(luciole::mixedEndingsNotice(LineEnding::Lf).contains("LF"));
    TEST_CHECK(luciole::mixedEndingsNotice(LineEnding::Cr).contains("CR"));
  }

  // Exactly the limit fits; one byte more is only part of the file.
  void testSizeLimit() {
    const Fixture fixture;
    std::string full(luciole::editing::kEditByteLimit, 'a');
    for (std::size_t at = 1000; at < full.size(); at += 1000) {
      full[at] = '\n';
    }
    TEST_CHECK(read(fixture.write("full.txt", full)).has_value());
    TEST_CHECK(refusal(fixture.write("over.txt", full + "b")) == luciole::kTooLargeToEdit);
  }

  void testEachRefusalIsOneSentence() {
    const Fixture fixture;
    const auto sentence = [](const std::string& text) {
      TEST_CHECK(!text.empty() && text.ends_with('.') && !text.contains('\n'));
      return text;
    };
    TEST_CHECK(sentence(refusal(fixture.write("latin1.txt", "caf\xE9\n"))).contains("UTF-8"));
    TEST_CHECK(
        sentence(refusal(fixture.write("long.txt", "a\n" + std::string(16 * 1024 + 1, 'x') + "\n"))).contains("16 KiB")
    );
    const std::string key = "-----BEGIN OPENSSH PRIVATE KEY-----\nAAAA\n-----END OPENSSH PRIVATE KEY-----\n";
    TEST_CHECK(sentence(refusal(fixture.write("id_test", key))).contains("private key"));
    TEST_CHECK(sentence(refusal(fixture.write("nul.txt", std::string("a\0b\n", 4)))).contains("NUL"));
    TEST_CHECK(sentence(refusal(fixture.write("locked.txt", "text\n", 0444))).contains("read-only"));
    const auto first = fixture.write("first.txt", "text\n");
    fs::create_hard_link(first, fixture.directory / "second.txt");
    TEST_CHECK(sentence(refusal(first)).contains("hard link"));
    const auto folder = fixture.directory / "locked";
    fs::create_directory(folder);
    const auto inside = fixture.write("locked/inside.txt", "text\n");
    TEST_CHECK(::chmod(folder.c_str(), 0555) == 0);
    const auto lockedFolder = refusal(inside);
    TEST_CHECK(::chmod(folder.c_str(), 0755) == 0);
    TEST_CHECK(sentence(lockedFolder).contains("folder") && lockedFolder.contains("read-only"));
    // The checks that need no read come first.
    TEST_CHECK(refusal(fixture.write("locked-latin1.txt", "caf\xE9\n", 0444)).contains("read-only"));
    const auto remote = luciole::readEditableText(Location::fromUri("sftp://example.invalid/notes.txt"), {});
    TEST_CHECK(!remote && sentence(remote.error()).contains("local"));
    TEST_CHECK(sentence(refusal(fixture.directory / "missing.txt")).size() > 1);
  }

  void testSaveSentences() {
    TEST_CHECK(luciole::saveSentence({.status = Status::Saved, .identity = std::nullopt, .message = {}}).empty());
    for (const auto status :
         {Status::SavedNotFlushed, Status::ChangedOnDisk, Status::ReadOnlyFile, Status::ReadOnlyFolder,
          Status::HardLinked, Status::NotRegularFile, Status::OwnerNotKept, Status::AttributesNotKept, Status::NoSpace,
          Status::Cancelled, Status::NotLocal, Status::Failed}) {
      const auto text = luciole::saveSentence({.status = status, .identity = std::nullopt, .message = "Disk error"});
      TEST_CHECK(!text.empty() && text.ends_with('.') && !text.contains('\n'));
    }
    const auto changed = luciole::saveSentence({.status = Status::ChangedOnDisk, .identity = {}, .message = {}});
    TEST_CHECK(changed.contains("changed on disk"));
    const auto failed = luciole::saveSentence({.status = Status::Failed, .identity = {}, .message = "Disk error"});
    TEST_CHECK(failed.contains("Disk error"));
  }

  // The worker answers on the main context; a cancelled read answers too, with nothing to edit.
  void testWorkerRead() {
    const Fixture fixture;
    const auto path = fixture.write("notes.txt", "hello\n");
    std::optional<std::expected<luciole::EditableText, std::string>> got;
    luciole::startEditableTextRead(at(path), {}, [&](auto result) { got.emplace(std::move(result)); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!got && std::chrono::steady_clock::now() < deadline) {
      g_main_context_iteration(nullptr, FALSE);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(got && *got && (*got)->buffer.text() == "hello\n");
    CancelToken cancelled;
    cancelled.cancel();
    got.reset();
    luciole::startEditableTextRead(at(path), cancelled, [&](auto result) { got.emplace(std::move(result)); });
    while (!got && std::chrono::steady_clock::now() < deadline) {
      g_main_context_iteration(nullptr, FALSE);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(got && !*got);
  }
} // namespace

int main() {
  if (::geteuid() == 0) {
    return 77; // root writes read-only files, so those refusals cannot be shown
  }
  testWholeTextKeepsItsBytes();
  testMixedEndingsAreNamed();
  testSizeLimit();
  testEachRefusalIsOneSentence();
  testSaveSentences();
  testWorkerRead();
  std::println("preview-edit-text: all checks passed");
  return 0;
}
