#include "app/gtk_color_watch.h"
#include "app/palette_watcher.h"
#include "temp_folder.h"
#include "test_check.h"
#include "ui/palette.h"
#include "ui/palette_file.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <glib.h>
#include <print>
#include <string_view>
#include <vector>

// The real watcher on a real main loop, in a private config home: the desktop's GTK colors, colors.toml over them,
// and the way back.
namespace {
  namespace fs = std::filesystem;
  using luciole::ColorScheme;

  void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    TEST_CHECK(file.good());
  }

  // Runs the main loop until the check holds; a check that never holds fails after ten seconds.
  void waitFor(const std::function<bool()>& holds, std::string_view what, int limitMs = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limitMs);
    while (!holds()) {
      if (std::chrono::steady_clock::now() > deadline) {
        std::println(stderr, "timed out waiting for {}", what);
        TEST_CHECK(false);
      }
      if (!g_main_context_iteration(nullptr, FALSE)) {
        g_usleep(5000);
      }
    }
  }

  // Lets the loop run so a change that must not happen has the chance to.
  void settle() {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < until) {
      if (!g_main_context_iteration(nullptr, FALSE)) {
        g_usleep(5000);
      }
    }
  }

  std::string gtkCss(std::string_view surface, std::string_view accent) {
    return std::format(
        "@define-color window_bg_color {};\n@define-color window_fg_color #eeeeee;\n"
        "@define-color accent_bg_color {};\n",
        surface, accent
    );
  }

  void testFollowsDesktopColorsAndColorFile() {
    const luciole::test::TempFolder folder("palette-watcher");
    const auto config = folder.path() / "config";
    fs::create_directories(config);
    TEST_CHECK(::setenv("XDG_CONFIG_HOME", config.c_str(), 1) == 0);
    const auto gtk = config / "gtk-4.0";
    const auto toml = config / "luciole" / "colors.toml";

    luciole::PaletteWatcher watcher(ColorScheme::Auto);
    const auto builtIn = luciole::defaultPalette(ColorScheme::Auto);
    TEST_CHECK(::palette == builtIn); // neither folder exists yet

    // A stylesheet that imports another, written where no folder existed.
    write(gtk / "noctalia.css", gtkCss("#112233", "#ff8800"));
    write(gtk / "gtk.css", "@import url(\"noctalia.css\");\n");
    waitFor(
        [&] { return ::palette.surface == hex("#112233") && ::palette.primary == hex("#ff8800"); }, "desktop colors"
    );

    // The tool rewrites the imported file on every apply.
    write(gtk / "noctalia.css", gtkCss("#223344", "#00aa55"));
    waitFor([&] { return ::palette.surface == hex("#223344") && ::palette.primary == hex("#00aa55"); }, "rewrite");

    // colors.toml wins while it exists.
    write(toml, "[colors]\nsurface = \"#010101\"\n");
    waitFor([&] { return ::palette.surface == hex("#010101"); }, "colors.toml");
    write(gtk / "noctalia.css", gtkCss("#334455", "#aa0055"));
    settle();
    TEST_CHECK(::palette.surface == hex("#010101")); // desktop changes do not touch it

    // Removed, the desktop colors return, including what changed meanwhile.
    TEST_CHECK(fs::remove(toml));
    waitFor([&] { return ::palette.surface == hex("#334455"); }, "colors.toml removed");

    // An explicit scheme ignores the desktop colors.
    watcher.setScheme(ColorScheme::Light);
    TEST_CHECK(::palette == luciole::defaultPalette(ColorScheme::Light));
    write(gtk / "noctalia.css", gtkCss("#445566", "#aa0055"));
    settle();
    TEST_CHECK(::palette == luciole::defaultPalette(ColorScheme::Light));
    watcher.setScheme(ColorScheme::Auto);
    TEST_CHECK(::palette.surface == hex("#445566"));

    // The stylesheet moves its import to a folder with a space in its name: that folder is watched from then on.
    const auto themes = folder.path() / "my themes";
    write(themes / "theme.css", gtkCss("#556677", "#00ccff"));
    write(gtk / "gtk.css", std::format("@import url(\"file://{}/my%20themes/theme.css\");\n", folder.path().string()));
    waitFor([&] { return ::palette.surface == hex("#556677"); }, "moved import");
    write(themes / "theme.css", gtkCss("#667788", "#00ccff"));
    waitFor([&] { return ::palette.surface == hex("#667788"); }, "rewrite of the moved import");

    // Without gtk.css the built-in palette returns.
    TEST_CHECK(fs::remove(gtk / "gtk.css"));
    waitFor([&] { return ::palette == builtIn; }, "gtk.css removed");
  }

  // A tool writes gtk.css, then its own sheet, in a folder that exists, in one made later, and through a link whose
  // target is rewritten in place. The palette follows each.
  void testImportsThatAppearLater() {
    const luciole::test::TempFolder folder("palette-watcher-late");
    const auto config = folder.path() / "config";
    fs::create_directories(config);
    TEST_CHECK(::setenv("XDG_CONFIG_HOME", config.c_str(), 1) == 0);
    const auto gtk = config / "gtk-4.0";
    luciole::PaletteWatcher watcher(ColorScheme::Auto);
    const auto builtIn = luciole::defaultPalette(ColorScheme::Auto);

    write(gtk / "gtk.css", "@import url(\"noctalia.css\");\n");
    settle();
    TEST_CHECK(::palette == builtIn);
    write(gtk / "noctalia.css", gtkCss("#112233", "#ff8800"));
    waitFor([&] { return ::palette.surface == hex("#112233"); }, "sheet written after gtk.css");

    write(gtk / "gtk.css", "@import url(\"../themes/later.css\");\n");
    waitFor([&] { return ::palette == builtIn; }, "import of a missing folder");
    settle();
    fs::create_directories(config / "themes");
    settle();
    write(config / "themes" / "later.css", gtkCss("#223344", "#00aa55"));
    waitFor([&] { return ::palette.surface == hex("#223344"); }, "sheet in a folder made later");

    const auto dots = folder.path() / "dots";
    write(dots / "real.css", gtkCss("#334455", "#aa0055"));
    fs::create_symlink(dots / "real.css", gtk / "linked.css");
    write(gtk / "gtk.css", "@import url(\"linked.css\");\n");
    waitFor([&] { return ::palette.surface == hex("#334455"); }, "linked sheet");
    write(dots / "real.css", gtkCss("#445566", "#aa0055"));
    waitFor([&] { return ::palette.surface == hex("#445566"); }, "edit of a link's target");
  }

  // A folder replaced by another at the same path, in less time than the debounce: a later change in the new
  // folder shows without waiting for the provider to find the folder again.
  void testReplacedFolders() {
    const luciole::test::TempFolder folder("palette-watcher-replaced");
    const auto config = folder.path() / "config";
    fs::create_directories(config);
    TEST_CHECK(::setenv("XDG_CONFIG_HOME", config.c_str(), 1) == 0);
    const auto gtk = config / "gtk-4.0";
    write(gtk / "gtk.css", gtkCss("#112233", "#ff8800"));
    luciole::PaletteWatcher watcher(ColorScheme::Auto);
    TEST_CHECK(::palette.surface == hex("#112233"));

    // The old folder stays until the new one exists, so the two never share an inode number.
    fs::rename(gtk, config / "gtk-old");
    write(gtk / "gtk.css", gtkCss("#223344", "#00aa55"));
    fs::remove_all(config / "gtk-old");
    waitFor([&] { return ::palette.surface == hex("#223344"); }, "replaced folder");
    write(gtk / "gtk.css", gtkCss("#334455", "#00aa55"));
    waitFor([&] { return ::palette.surface == hex("#334455"); }, "rewrite in the replaced folder", 2500);
  }

  // gtk-4.0 is a link that a dotfile manager repoints; the next reload watches the new target.
  void testRepointedLink() {
    const luciole::test::TempFolder folder("palette-watcher-repointed");
    const auto config = folder.path() / "config";
    fs::create_directories(config);
    TEST_CHECK(::setenv("XDG_CONFIG_HOME", config.c_str(), 1) == 0);
    const auto first = folder.path() / "dots-a";
    const auto second = folder.path() / "dots-b";
    write(first / "gtk.css", gtkCss("#445566", "#00aa55"));
    write(second / "gtk.css", gtkCss("#556677", "#00aa55"));
    fs::create_directory_symlink(first, config / "gtk-4.0");
    luciole::PaletteWatcher watcher(ColorScheme::Auto);
    TEST_CHECK(::palette.surface == hex("#445566"));

    fs::remove(config / "gtk-4.0");
    fs::create_directory_symlink(second, config / "gtk-4.0");
    watcher.setScheme(ColorScheme::Light);
    watcher.setScheme(ColorScheme::Auto);
    TEST_CHECK(::palette.surface == hex("#556677"));
    write(second / "gtk.css", gtkCss("#667788", "#00aa55"));
    waitFor([&] { return ::palette.surface == hex("#667788"); }, "rewrite behind the repointed link", 2500);
  }

  // A folder reached through a linked parent is one folder.
  void testLinkedParentIsWatchedOnce() {
    const luciole::test::TempFolder folder("gtk-color-watch-linked");
    const auto real = folder.path() / "real";
    const auto link = folder.path() / "link";
    write(real / "gtk-4.0" / "gtk.css", "");
    fs::create_directory_symlink(real, link);
    const auto root = link / "gtk-4.0" / "gtk.css";
    luciole::GtkColorWatch watch([] {});
    watch.follow(root, {root});
    TEST_CHECK((watch.folders() == std::vector<fs::path>{link / "gtk-4.0"}));
  }

  // Asking again for the same folders is no change.
  void testStableFoldersReportNoChange() {
    const luciole::test::TempFolder folder("gtk-color-watch-stable");
    const auto root = folder.path() / "gtk.css";
    write(root, "");
    luciole::GtkColorWatch watch([] {});
    TEST_CHECK(watch.follow(root, {root}));
    TEST_CHECK(!watch.follow(root, {root}));
  }

  void testWatchedFolders() {
    const luciole::test::TempFolder folder("gtk-color-watch");
    int changes = 0;
    luciole::GtkColorWatch watch([&changes] { ++changes; });
    const auto root = folder.path() / "missing" / "deeper" / "gtk.css";

    watch.follow(root, {});
    TEST_CHECK((watch.folders() == std::vector<fs::path>{folder.path()})); // the closest folder that exists

    std::vector<fs::path> files{root};
    for (int i = 0; i < 12; ++i) {
      const auto path = folder.path() / std::format("dir{}", i) / "x.css";
      write(path, "");
      files.push_back(path);
    }
    watch.follow(root, files);
    TEST_CHECK(watch.folders().size() == luciole::GtkColorWatch::kMostFolders);
    TEST_CHECK(watch.folders().front() == folder.path());

    watch.follow(root, {files[1], files[2]});
    TEST_CHECK(
        (watch.folders() == std::vector<fs::path>{folder.path(), folder.path() / "dir0", folder.path() / "dir1"})
    );
    watch.follow({}, {});
    TEST_CHECK(watch.folders().empty());

    // Only the files read, or the folder a missing root waits for, call back; other files in a folder do not.
    watch.follow(files[1], {files[1]});
    write(files[1].parent_path() / "other.css", "x");
    settle();
    TEST_CHECK(changes == 0);
    write(files[1], "@define-color a #111111;");
    waitFor([&] { return changes > 0; }, "a change of a file read");
  }
} // namespace

int main() {
  testImportsThatAppearLater();
  testWatchedFolders();
  testLinkedParentIsWatchedOnce();
  testStableFoldersReportNoChange();
  testReplacedFolders();
  testRepointedLink();
  testFollowsDesktopColorsAndColorFile();
  std::println("palette watcher checks passed");
  return 0;
}
