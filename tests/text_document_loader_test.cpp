#include "fs/gio_ptr.h"
#include "fs/location.h"
#include "imaging/text_document_loader.h"
#include "temp_folder.h"
#include "test_check.h"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace {
  using namespace luciole;
  using namespace std::string_literals;
  using namespace std::string_view_literals;
  using formats::Language;
  using formats::LineEnding;
  using State = TextDocumentState;
  namespace fs = std::filesystem;

  const UserFileIdentity kIdentity{.device = 7, .inode = 42, .size = 1234};

  UserFileContent content(std::string bytes, bool truncated = false) {
    return {.bytes = std::move(bytes), .truncated = truncated, .identity = kIdentity};
  }

  TextDocument made(std::string bytes, std::string_view name, std::string_view type = {}, bool truncated = false) {
    return textDocument(content(std::move(bytes), truncated), name, type);
  }

  TextDocument failed(FileErrorKind kind, std::string message = "Permission denied") {
    return textDocument(std::unexpected(FileError{.kind = kind, .message = std::move(message)}), "a.txt", {});
  }

  std::size_t swatchCount(const formats::StyledDocument& document) {
    std::size_t count = 0;
    for (const auto& block : document.blocks) {
      for (const auto& line : block.lines) {
        count += line.swatches.size();
      }
    }
    return count;
  }

  void testCodeIsShown() {
    const auto css = made(".a {\n  color: #ff0000;\n}\n", "style.css");
    TEST_CHECK(css.state == State::Shown);
    TEST_CHECK(css.document != nullptr && swatchCount(*css.document) == 1);
    TEST_CHECK(css.kind.language == Language::Css);
    TEST_CHECK(css.facts.lines == 3 && css.facts.bytes == 25 && !css.facts.cut);
    TEST_CHECK(css.facts.text.lineEnding == LineEnding::Lf && css.facts.text.validUtf8 && css.facts.text.finalNewline);
    TEST_CHECK(css.identity == kIdentity && css.message.empty());
  }

  void testEncodingAndEndings() {
    const auto marked = made("\xEF\xBB\xBFone\r\ntwo\r\nthree", "notes.txt");
    TEST_CHECK(marked.state == State::Shown);
    TEST_CHECK(marked.facts.text.byteOrderMark && marked.facts.text.lineEnding == LineEnding::CrLf);
    TEST_CHECK(!marked.facts.text.finalNewline && marked.facts.lines == 3);
    const auto latin = made("caf\xE9\n", "notes.txt");
    TEST_CHECK(latin.state == State::Shown && !latin.facts.text.validUtf8);
    const auto empty = made("", "empty.txt");
    TEST_CHECK(empty.state == State::Shown && empty.facts.bytes == 0 && empty.facts.lines == 0);
    TEST_CHECK(empty.document != nullptr);
  }

  void testCut() {
    // A read cut inside a character keeps the facts of the whole characters before it.
    const auto partial = made("abc\ndef \xE2\x82", "notes.txt", {}, true);
    TEST_CHECK(partial.state == State::Cut && partial.facts.cut);
    TEST_CHECK(partial.facts.text.validUtf8 && partial.facts.lines == 2);
    TEST_CHECK(formats::joinedText(partial.document->blocks[0].lines.back()) == "def ");
    std::string many;
    for (int i = 0; i < 2500; ++i) {
      many += "line\n";
    }
    const auto tall = made(many, "many.log");
    TEST_CHECK(tall.state == State::Cut && tall.facts.cut && tall.facts.lines == 2500);
    TEST_CHECK(tall.document->linesCapped && tall.identity == kIdentity);
  }

  void testKindHints() {
    const auto markdown = made("# Title\n", "README", "text/markdown");
    TEST_CHECK(markdown.kind.language == Language::Plain); // the name decides first
    const auto hinted = made("# Title\n", "notes", "text/markdown");
    TEST_CHECK(hinted.state == State::Shown && hinted.kind.language == Language::Markdown);
    const auto json = made("{\"a\":1}", "data", "application/ld+json");
    TEST_CHECK(json.kind.language == Language::Json);
    const auto shell = made("echo hi\n", "run", "application/x-shellscript");
    TEST_CHECK(shell.kind.language == Language::Shell);
    const auto plain = made("just words\n", "words", "text/x-unknown-thing");
    TEST_CHECK(plain.state == State::Shown && plain.kind.language == Language::Plain);
    // Neither the name nor the type says text: the bytes do.
    const auto sniffed = made("hello\n", "blob", "application/octet-stream");
    TEST_CHECK(sniffed.state == State::Shown && sniffed.kind.language == Language::Plain);
  }

  void testNotText() {
    const auto binary = made("ELF\x00\x01\x02"s, "blob", "application/octet-stream");
    TEST_CHECK(binary.state == State::NotText && binary.document != nullptr);
    TEST_CHECK(binary.document->blocks.size() == 1 && binary.document->blocks[0].lines.size() == 1);
    TEST_CHECK(formats::joinedText(binary.document->blocks[0].lines[0]).starts_with("00000000  45 4c 46 00"));
    TEST_CHECK(binary.identity == kIdentity && !binary.facts.cut);
    const auto named = made("a\x00z"s, "notes.txt");
    TEST_CHECK(named.state == State::NotText);
    const auto picture = made("plain", "photo.png", "image/png");
    TEST_CHECK(picture.state == State::NotText);
    const auto large = made(std::string(70 * 1024, '\x01'), "blob", {}, true);
    TEST_CHECK(large.state == State::NotText && large.facts.cut && large.document->bytesCapped);
  }

  void testRefusals() {
    const auto key =
        made("-----BEGIN OPENSSH PRIVATE KEY-----\nabc\n-----END OPENSSH PRIVATE KEY-----\n", "id_ed25519");
    TEST_CHECK(key.state == State::PrivateKey && key.document == nullptr && !key.identity);
    const auto changed = failed(FileErrorKind::Changed);
    TEST_CHECK(changed.state == State::Changed && changed.document == nullptr);
    const auto denied = failed(FileErrorKind::Denied);
    TEST_CHECK(denied.state == State::Unreadable && denied.message == "Permission denied" && !denied.identity);
    const auto missing = failed(FileErrorKind::Missing, "No such file or directory");
    TEST_CHECK(missing.state == State::Unreadable && missing.message == "No such file or directory");
    TEST_CHECK(failed(FileErrorKind::Cancelled).state == State::Cancelled);
  }

  // A private folder.
  struct Folder {
    luciole::test::TempFolder tempFolder{"luciole-text-document"};
    fs::path path = tempFolder.path();

    fs::path write(std::string_view name, std::string_view bytes) const {
      const auto file = path / name;
      std::ofstream(file, std::ios::binary) << bytes;
      return file;
    }
  };

  TextDocument loaded(const fs::path& file, std::string_view type, const CancelToken& token = {}) {
    std::optional<TextDocument> result;
    startTextDocumentLoad(
        Location::fromPath(file.native()), file.filename().string(), std::string(type), token,
        [&](TextDocument document) { result = std::move(document); }
    );
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!result) {
      while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
      }
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::move(*result);
  }

  void testLoadsFromDisk() {
    const Folder folder;
    const auto json = folder.write("data.json", "{\"name\":\"x\",\"list\":[1,2]}\n");
    const auto shown = loaded(json, "application/json");
    TEST_CHECK(shown.state == State::Shown && shown.kind.language == Language::Json);
    TEST_CHECK(shown.identity && shown.identity->size == 26);
    TEST_CHECK(shown.document->blocks.size() == 1 && shown.document->blocks[0].lines.size() > 3);
    const auto big = folder.write("big.txt", std::string(300 * 1024, 'a'));
    const auto cut = loaded(big, "text/plain");
    TEST_CHECK(cut.state == State::Cut && cut.facts.bytes == kTextDocumentReadBytes);
    TEST_CHECK(cut.identity && cut.identity->size == 300 * 1024);
    TEST_CHECK(loaded(folder.path / "missing.txt", "text/plain").state == State::Unreadable);
    const CancelToken cancelled;
    cancelled.cancel();
    TEST_CHECK(loaded(json, "application/json", cancelled).state == State::Cancelled);
  }
} // namespace

int main() {
  testCodeIsShown();
  testEncodingAndEndings();
  testCut();
  testKindHints();
  testNotText();
  testRefusals();
  testLoadsFromDisk();
  std::println("text-document-loader: all checks passed");
  return 0;
}
