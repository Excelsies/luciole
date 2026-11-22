#include "event_loop/glib_poll_source.h"
#include "fs/directory_session.h"
#include "temp_folder.h"
#include "test_check.h"
#include "thumbnails/thumbnail_codec.h"
#include "thumbnails/thumbnail_service.h"
#include "thumbnails/thumbnail_store.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <malloc.h>
#include <memory>
#include <poll.h>
#include <print>
#include <unistd.h>
#include <vector>

// How long a thumbnail the view let go of stays in memory, and whether the memory it held goes back to the system.
namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;

  class Pump {
  public:
    void until(const std::function<bool()>& done, std::chrono::milliseconds limit = 30s) {
      const auto deadline = std::chrono::steady_clock::now() + limit;
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) >= 0);
        m_source.dispatch(fds, start);
      }
    }
    void run(std::chrono::milliseconds span) {
      const auto end = std::chrono::steady_clock::now() + span;
      until([&] { return std::chrono::steady_clock::now() >= end; }, span + 30s);
    }

  private:
    GlibPollSource m_source;
  };

  // Resident bytes of this process.
  std::size_t resident() {
    std::ifstream stream("/proc/self/statm");
    std::size_t size = 0;
    std::size_t pages = 0;
    stream >> size >> pages;
    TEST_CHECK(static_cast<bool>(stream));
    return pages * static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  }

  // A picture of noise a PNG cannot shrink much, so decoding it fills real pages.
  LoadedImageFile noise(int width, int height, unsigned seed) {
    LoadedImageFile image{
        .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4),
        .width = width,
        .height = height
    };
    unsigned value = seed;
    for (auto& byte : image.rgba) {
      value = value * 1664525U + 1013904223U;
      byte = static_cast<std::uint8_t>(value >> 24U);
    }
    return image;
  }

  void write(const fs::path& path, const LoadedImageFile& image) {
    const auto bytes = luciole::writeThumbnailPng({.image = image, .text = {}});
    TEST_CHECK(bytes.has_value());
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    TEST_CHECK(static_cast<bool>(stream));
  }

  struct Folder {
    luciole::test::TempFolder temp{"luciole-thumbnail-retention"};
    fs::path root = temp.path();
    Folder() {
      fs::create_directory(root / "home");
      ::setenv("HOME", (root / "home").c_str(), 1);
      ::setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);
      ::setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
      ::setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
      ::setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/luciole-thumbnail-test-no-session", 1);
      ::setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/luciole-thumbnail-test-no-system", 1);
    }
  };

  constexpr int kPictures = 240;
  constexpr int kWidth = 128;
  constexpr int kHeight = 96;
  constexpr std::size_t kThumbnailBytes = static_cast<std::size_t>(kWidth) * kHeight * 4;

  std::vector<luciole::DirectoryEntry> entries(const Folder& folder, Pump& pump) {
    luciole::DirectorySession session;
    session.open(luciole::Location::fromPath(folder.root.native()));
    pump.until([&] { return session.snapshot().status != luciole::DirectoryStatus::Loading; });
    TEST_CHECK(session.snapshot().status == luciole::DirectoryStatus::Ready);
    std::vector<luciole::DirectoryEntry> found;
    for (const auto& entry : session.snapshot().entries) {
      if (entry.name.starts_with("picture-")) {
        found.push_back(entry);
      }
    }
    TEST_CHECK(found.size() == kPictures);
    return found;
  }

  // The service keeps a released thumbnail for its idle time and then lets it go, with the memory it held:
  // a freed block of the heap is of no use to the system until the allocator gives it back.
  void evictedThumbnailsGiveTheirMemoryBack() {
    Folder folder;
    for (int index = 0; index < kPictures; ++index) {
      write(folder.root / std::format("picture-{:03}.png", index), noise(8, 8, static_cast<unsigned>(index)));
    }
    Pump pump;
    const auto found = entries(folder, pump);
    // The thumbnails are in the disk cache already, as after an earlier visit: no decoder runs, only the reads.
    luciole::ThumbnailStore store;
    for (std::size_t index = 0; index < found.size(); ++index) {
      store.write(
          {.image = noise(kWidth, kHeight, static_cast<unsigned>(index)),
           .text =
               {{"Thumb::URI", found[index].location().uri()},
                {"Thumb::MTime", std::to_string(*found[index].modifiedSeconds)},
                {"Thumb::Size", std::to_string(*found[index].size)}}},
          kWidth, 1ULL << 30, nullptr
      );
    }
    luciole::ThumbnailService thumbnails;
    thumbnails.setRetention({.thumbnail = 400ms, .preview = 400ms, .sweep = 100ms});
    const auto before = resident();
    std::vector<std::shared_ptr<const luciole::FileIconImage>> held;
    for (const auto& entry : found) {
      auto image = thumbnails.request(entry, kWidth);
      TEST_CHECK(image != nullptr);
      held.push_back(std::move(image));
    }
    // Small blocks the process keeps for good land between the thumbnails, as the app's own objects do, so a freed
    // thumbnail is a hole in the heap and not its end.
    std::vector<std::unique_ptr<char[]>> pins;
    pump.until([&] {
      for (int i = 0; i < 16; ++i) {
        pins.push_back(std::make_unique<char[]>(200));
      }
      return std::ranges::all_of(held, [](const auto& image) { return image->ready; });
    });
    TEST_CHECK(thumbnails.reservedBytes() >= kPictures * (kThumbnailBytes / 2));
    const auto full = resident();
    held.clear();
    thumbnails.endFrame();
    pump.until([&] { return thumbnails.reservedBytes() == 0; });
    // The allocator is asked once the loop has nothing else to do.
    pump.run(500ms);
    const auto after = resident();
    // Most of what the pictures held is no longer resident: the part still in use may not be less than `before`.
    const std::size_t held_bytes = kPictures * kThumbnailBytes;
    TEST_CHECK(full > before + held_bytes / 2);
    TEST_CHECK(after < full - held_bytes / 2);
  }
} // namespace

int main() {
  // The app's allocator settings (src/app/application.cpp): one or two arenas, big blocks mapped on their own.
  (void)mallopt(M_ARENA_MAX, 1);
  (void)mallopt(M_MMAP_THRESHOLD, 128 * 1024);
  evictedThumbnailsGiveTheirMemoryBack();
  std::println("thumbnail-retention: all checks passed");
  return 0;
}
