#pragma once

#include "browser/workbench_services.h"
#include "event_loop/glib_poll_source.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <poll.h>
#include <span>
#include <vector>

// What the tests of the Workbench's walks share: a pump for the main context, folder trees, and a page's walks that
// update when the cache notifies.
namespace luciole::test::walks {
  namespace fs = std::filesystem;
  using luciole::Location;
  using luciole::MeasureCache;
  using luciole::WorkbenchWalks;

  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        once();
      }
    }
    void once() {
      std::vector<pollfd> fds;
      const auto start = m_source.addPollFds(fds);
      const int requested = m_source.pollTimeoutMs();
      TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) >= 0);
      m_source.dispatch(fds, start);
    }

  private:
    GlibPollSource m_source;
  };

  // `folders` folders of `files` files each.
  Location tree(const fs::path& root, int folders, int files) {
    for (int d = 0; d < folders; ++d) {
      const auto folder = root / std::format("d{:02}", d);
      fs::create_directories(folder);
      for (int f = 0; f < files; ++f) {
        std::ofstream(folder / std::format("f{}", f)) << "0123456789";
      }
    }
    return Location::fromPath(root.native());
  }

  luciole::MeasureServiceOptions quickPublication(std::size_t workers = 2) {
    luciole::MeasureServiceOptions options;
    options.publishInterval = std::chrono::milliseconds(1);
    options.workers = workers;
    return options;
  }

  // Updates the walks whenever the cache notifies, as a page's model does.
  class Driven {
  public:
    explicit Driven(MeasureCache& cache) : walks(cache, [this] { walks.update(); }), m_cache(cache) {
      m_listener = cache.addListener([this] { walks.update(); });
    }
    ~Driven() { m_cache.removeListener(m_listener); }
    Driven(const Driven&) = delete;
    Driven& operator=(const Driven&) = delete;
    void enter(const Location& folder, std::span<const Location> ancestors = {}) {
      walks.enter(folder, ancestors);
      walks.update();
    }
    // Walks queued or running for this page.
    std::size_t walking() const {
      return static_cast<std::size_t>(std::ranges::count_if(walks.walks(), [](const WorkbenchWalks::Walk& walk) {
        return walk.started && !walk.finished;
      }));
    }
    WorkbenchWalks walks;

  private:
    MeasureCache& m_cache;
    std::uint64_t m_listener = 0;
  };

} // namespace luciole::test::walks
