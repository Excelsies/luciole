#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "imaging/pixbuf_image_decoder.h"
#include "system/icon_resolver.h"
#include "temp_folder.h"
#include "test_check.h"
#include "thumbnails/file_icon_cache.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <png.h>
#include <print>
#include <stdexcept>

namespace {
  namespace fs = std::filesystem;
  struct Fixture {
    luciole::test::TempFolder tempFolder{"luciole-icons"};
    fs::path root = tempFolder.path();
    Fixture() {
      fs::create_directories(root / "config/gtk-3.0");
      fs::create_directories(root / "data/icons/Test/scalable/apps");
      fs::create_directories(root / "data/icons/Test/16x16/apps");
      fs::create_directories(root / "data/icons/Test/64x64/apps");
      fs::create_directories(root / "data/icons/Parent/scalable/apps");
      std::ofstream(root / "config/gtk-3.0/settings.ini") << "[Settings]\ngtk-icon-theme-name=Test\n";
      std::ofstream(root / "data/icons/Test/index.theme")
          << "[Icon Theme]\nName=Test\nInherits=Parent\nDirectories=scalable/apps,16x16/apps,64x64/apps\n"
             "[scalable/apps]\nSize=48\nType=Scalable\nMinSize=16\nMaxSize=256\n"
             "[16x16/apps]\nSize=16\nType=Fixed\n[64x64/apps]\nSize=64\nType=Fixed\n";
      std::ofstream(root / "data/icons/Parent/index.theme")
          << "[Icon Theme]\nName=Parent\nDirectories=scalable/apps\n[scalable/apps]\nSize=48\nType=Scalable\n";
      for (const auto* name : {"folder", "text-x-generic", "custom-app"}) {
        std::ofstream(root / "data/icons/Test/scalable/apps" / (std::string(name) + ".svg"))
            << "<svg xmlns='http://www.w3.org/2000/svg' width='48' height='48'>"
               "<path fill='#00cc44' d='M0 0h48v48H0z'/></svg>";
      }
      fs::copy_file(
          root / "data/icons/Test/scalable/apps/folder.svg", root / "data/icons/Parent/scalable/apps/inherited.svg"
      );
      std::ofstream(root / "data/icons/Test/scalable/apps/broken.svg") << "broken SVG";
      std::ofstream(root / "launcher.desktop") << "[Desktop Entry]\nType=Application\nIcon=custom-app\nExec=false\n";
      for (const int size : {16, 64}) {
        png_image image{};
        image.version = PNG_IMAGE_VERSION;
        image.width = image.height = size;
        image.format = PNG_FORMAT_RGBA;
        const std::vector<unsigned char> pixels(static_cast<std::size_t>(size) * size * 4U, 255);
        TEST_CHECK(
            png_image_write_to_file(
                &image,
                (root / ("data/icons/Test/" + std::to_string(size) + "x" + std::to_string(size) + "/apps/bitmap.png"))
                    .c_str(),
                0, pixels.data(), 0, nullptr
            )
            != 0
        );
      }
      g_setenv("HOME", root.c_str(), TRUE);
      g_setenv("XDG_CONFIG_HOME", (root / "config").c_str(), TRUE);
      g_setenv("XDG_DATA_HOME", (root / "data").c_str(), TRUE);
      g_setenv("XDG_DATA_DIRS", (root / "empty").c_str(), TRUE);
      g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
      g_setenv("GSETTINGS_SCHEMA_DIR", (root / "empty").c_str(), TRUE);
    }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int timeout = m_source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), timeout < 0 ? 20 : std::min(timeout, 20)) >= 0);
        m_source.dispatch(fds, start);
      }
    }

  private:
    GlibPollSource m_source;
  };
  // A cache that goes away leaves no worker running: every lookup of a started job is done when the destructor returns.
  void destroyingTheCacheWaitsForItsJobs() {
    constexpr int kNames = 1500;
    luciole::FileIconSource source;
    for (int index = 0; index < kNames; ++index) {
      source.names.push_back("absent-" + std::to_string(index));
    }
    const auto before = IconResolver::lookupCount();
    {
      luciole::FileIconCache cache;
      // Two jobs run at once; the third waits in the queue and is dropped with the cache.
      for (const int size : {20, 21, 22}) {
        cache.request(source, size);
      }
    }
    TEST_CHECK(IconResolver::lookupCount() - before == 2U * kNames);
  }
  void testIcons() {
    Fixture fixture;
    Pump pump;
    IconResolver resolver(true);
    TEST_CHECK(IconResolver::activeThemeName() == "Test");
    TEST_CHECK(resolver.resolve("inherited", 48).contains("/Parent/"));
    TEST_CHECK(resolver.resolve("bitmap", 24).contains("/64x64/"));
    TEST_CHECK(resolver.resolve("bitmap", 12).contains("/16x16/"));

    luciole::FileIconCache cache;
    const luciole::FileIconSource source{.names = {"broken", "missing", "folder"}, .filePath = {}, .desktopFile = {}};
    const auto first = cache.request(source, 48);
    TEST_CHECK(first == cache.request(source, 48));
    pump.until([&] { return first->ready; });
    TEST_CHECK(first->pixels && first->path.ends_with("folder.svg") && first->pixels->width == 48);
    TEST_CHECK(first->pixels->rgba[1] > first->pixels->rgba[0]);
    const auto large = cache.request(source, 96);
    pump.until([&] { return large->ready; });
    TEST_CHECK(large->pixels && large->pixels->width == 96);

    const luciole::GioPtr<GFileInfo> info(g_file_info_new());
    const luciole::GioPtr<GIcon> themed(g_themed_icon_new("inherited"));
    g_file_info_set_icon(info.get(), themed.get());
    g_file_info_set_name(info.get(), "launcher.desktop");
    g_file_info_set_file_type(info.get(), G_FILE_TYPE_REGULAR);
    const auto launcher =
        luciole::readFileIcon(info.get(), luciole::Location::fromPath((fixture.root / "launcher.desktop").native()));
    TEST_CHECK(launcher.names.front() == "inherited" && !launcher.desktopFile.empty());
    const auto app = cache.request(launcher, 48);
    pump.until([&] { return app->ready; });
    TEST_CHECK(app->path.ends_with("custom-app.svg"));

    const auto obsolete = cache.request(source, 80);
    cache.invalidate();
    const auto current = cache.request(source, 80);
    pump.until([&] { return current->ready; });
    TEST_CHECK(obsolete != current && !obsolete->ready);

    auto dying = std::make_unique<luciole::FileIconCache>();
    bool destroyed = false;
    dying->setChangedCallback([&] {
      dying.reset();
      destroyed = true;
    });
    const auto abandoned = dying->request(source, 32);
    pump.until([&] { return destroyed; });
    TEST_CHECK(abandoned->ready);
    destroyingTheCacheWaitsForItsJobs();
  }
} // namespace

int main() {
  luciole::installPixbufImageDecoder();
  try {
    testIcons();
    std::println("file icon checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "file icon test: {}", error.what());
    return 1;
  }
}
