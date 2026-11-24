#pragma once

#include "app/page/workbench_session.h"
#include "browser/workbench_services.h"
#include "event_loop/glib_poll_source.h"
#include "temp_folder.h"
#include "test_check.h"
#include "thumbnails/file_icon_cache.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glib.h>
#include <memory>
#include <poll.h>
#include <string>
#include <vector>

namespace luciole::test {
  namespace fs = std::filesystem;
  using luciole::Location;
  using luciole::WorkbenchSession;

  // base/home (the private home) and base/root: big/{b.bin 1000, deep/d.bin 3000}, small/t.txt 100, note.txt 50.
  class Fixture {
  public:
    Fixture() {
      root = base / "root";
      write(root / "big" / "b.bin", 1000);
      write(root / "big" / "deep" / "d.bin", 3000);
      write(root / "small" / "t.txt", 100);
      write(root / "note.txt", 50);
      fs::create_directories(base / "home");
      for (const char* variable : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_DATA_DIRS"}) {
        g_setenv(variable, (base / "home").c_str(), TRUE);
      }
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    Location at(const fs::path& relative) const {
      return Location::fromPath(relative.empty() ? root.native() : (root / relative).native());
    }

    luciole::test::TempFolder tempFolder{"luciole-workbench-session"};
    fs::path base = tempFolder.path();
    fs::path root;

  private:
    static void write(const fs::path& path, std::size_t bytes) {
      fs::create_directories(path.parent_path());
      std::ofstream(path) << std::string(bytes, 'x');
    }
  };

  // The page's side of a Workbench: its browser over the fixture, and a parent holding the file pane between two
  // siblings.
  struct Rig {
    Fixture fixture;
    GlibPollSource source;
    luciole::WorkbenchServices services;
    luciole::FileIconCache icons;
    luciole::BrowserController browser{{}, false};
    Node parent;
    Node* pane = nullptr;
    InputArea* focus = nullptr;

    Rig() {
      parent.addChild(std::make_unique<Node>());
      auto made = std::make_unique<Node>();
      made->setFlexGrow(1);
      focus = static_cast<InputArea*>(made->addChild(std::make_unique<InputArea>()));
      pane = parent.addChild(std::move(made));
      parent.addChild(std::make_unique<Node>());
      browser.open(fixture.at(""));
      until([this] { return browser.model().snapshot().status == luciole::DirectoryStatus::Ready; });
    }

    std::unique_ptr<WorkbenchSession> mount() {
      return std::make_unique<WorkbenchSession>(
          *pane, focus, services.measureCache(), icons, [] { return 0U; },
          luciole::WorkbenchModelOptions{.ancestorLevels = 0}
      );
    }

    void once() {
      std::vector<pollfd> fds;
      const auto start = source.addPollFds(fds);
      const int requested = source.pollTimeoutMs();
      TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 5 : std::min(requested, 5)) >= 0);
      source.dispatch(fds, start);
    }

    // At most 4000 rounds of at most 5 ms each.
    void until(const std::function<bool()>& done) {
      for (std::size_t round = 0; round < 4000 && !done(); ++round) {
        once();
      }
      TEST_CHECK(done());
    }

    void select(const std::string& name) {
      auto& model = browser.model();
      for (const auto id : model.rows()) {
        if (model.entry(id)->name == name) {
          TEST_CHECK(model.select(id));
          return;
        }
      }
      TEST_CHECK(false);
    }

    void frame(WorkbenchSession& session) {
      session.update(luciole::workbenchFeed(browser), luciole::WorkbenchChoices{});
    }

    bool idle() {
      const auto status = services.measureService().status();
      return status.queued == 0 && status.running == 0;
    }

    // Frames until the model has published, measures nothing and no walk is queued or running, three rounds in a row,
    // so every walk the session plans has been asked for; at most 4000 rounds.
    void settle(WorkbenchSession& session) {
      std::size_t quiet = 0;
      for (std::size_t round = 0; round < 4000 && quiet < 3; ++round) {
        frame(session);
        once();
        quiet = session.model().snapshot() && !session.model().measuring() && idle() ? quiet + 1 : 0;
      }
      TEST_CHECK(quiet == 3);
    }
  };
} // namespace luciole::test
