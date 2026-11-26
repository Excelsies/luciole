#include "browser/workbench_services.h"
#include "temp_folder.h"
#include "test_check.h"
#include "ui/workbench/column_preview_loads.h"
#include "ui/workbench/column_preview_view.h"

#include <archive.h>
#include <archive_entry.h>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <glib.h>
#include <memory>
#include <print>
#include <string>
#include <thread>

// An archive in a Workbench column is listed within the preview's own limits: one with more entries than the preview
// lists is refused with a sentence, never loaded.
namespace {
  namespace fs = std::filesystem;
  using luciole::ColumnPreviewKind;
  using luciole::ColumnPreviewLoad;
  using luciole::ColumnPreviewRequest;
  using luciole::ColumnPreviewStatus;
  using luciole::ColumnPreviewView;

  // A gzip-compressed tar of `count` empty files.
  fs::path writeTar(const fs::path& folder, const std::string& name, std::size_t count) {
    const auto path = folder / name;
    struct Writer {
      archive* handle = archive_write_new();
      ~Writer() { archive_write_free(handle); }
    } out;
    TEST_CHECK(archive_write_add_filter_gzip(out.handle) == ARCHIVE_OK);
    TEST_CHECK(archive_write_set_format_ustar(out.handle) == ARCHIVE_OK);
    TEST_CHECK(archive_write_open_filename(out.handle, path.c_str()) == ARCHIVE_OK);
    struct Entry {
      archive_entry* handle = archive_entry_new();
      ~Entry() { archive_entry_free(handle); }
    } entry;
    for (std::size_t i = 0; i < count; ++i) {
      archive_entry_clear(entry.handle);
      archive_entry_set_pathname(entry.handle, ("e" + std::to_string(i)).c_str());
      archive_entry_set_filetype(entry.handle, AE_IFREG);
      archive_entry_set_perm(entry.handle, 0644);
      archive_entry_set_size(entry.handle, 0);
      TEST_CHECK(archive_write_header(out.handle, entry.handle) == ARCHIVE_OK);
    }
    TEST_CHECK(archive_write_close(out.handle) == ARCHIVE_OK);
    return path;
  }

  struct Loaded {
    ColumnPreviewView view;
    std::unique_ptr<ColumnPreviewLoad> load;

    explicit Loaded(const fs::path& path) {
      luciole::DirectoryEntry entry;
      entry.place = luciole::Location::fromPath(path.string());
      entry.name = path.filename().string();
      entry.type = luciole::FileType::Regular;
      const ColumnPreviewRequest request{.entry = entry, .kind = ColumnPreviewKind::Archive};
      load = luciole::startColumnPreviewLoad(view, request, luciole::WorkbenchPreviewWorkers{}, [] {});
      TEST_CHECK(load != nullptr);
    }

    // Frames until the load is no longer loading, at most ten seconds.
    ColumnPreviewStatus settle() {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (load->status() == ColumnPreviewStatus::Loading && std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 64 && g_main_context_iteration(nullptr, FALSE) != FALSE; ++i) {
        }
        load->update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return load->status();
    }
  };

  void anArchiveWithinTheLimitsIsListed(const fs::path& folder) {
    Loaded loaded(writeTar(folder, "small.tar.gz", 3));
    TEST_CHECK(loaded.settle() == ColumnPreviewStatus::Shown);
    TEST_CHECK(loaded.load->facts().entries == 3);
  }

  // The preview lists at most 50000 entries.
  void anArchiveOverTheEntryLimitIsRefused(const fs::path& folder) {
    Loaded loaded(writeTar(folder, "many.tar.gz", 50001));
    TEST_CHECK(loaded.settle() == ColumnPreviewStatus::Failed);
    TEST_CHECK(loaded.view.message() == "This archive is too large to list.");
    TEST_CHECK(!loaded.load->facts().entries);
  }
} // namespace

int main() {
  const luciole::test::TempFolder scratch("luciole-archive-limits");
  anArchiveWithinTheLimitsIsListed(scratch.path());
  anArchiveOverTheEntryLimitIsRefused(scratch.path());
  std::println("column-preview-archive-limits: all checks passed");
  return 0;
}
