#include "app/page/workbench_folder_sizes.h"
#include "browser/browser_controller.h"
#include "test_check.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/browser/file_view.h"
#include "workbench_session_rig.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {
  using luciole::test::Rig;

  // The class keeps a list's folder cells in step with the session's measure: nothing is bound again while the measure
  // has not moved, a measure that changed a row's text binds that row again once and asks for one frame, and the
  // next call finds nothing to do until the measure moves again.
  void refreshBindsOnlyStaleRows() {
    Rig rig;
    auto session = rig.mount();
    luciole::ThumbnailService thumbnails;
    luciole::FileViewAdapter adapter(
        rig.browser, rig.icons, thumbnails, luciole::ViewMode::List, [] { return 0U; }, [] { return false; }
    );
    adapter.setFolderSizeEnabled(luciole::rowsMeasureFolders(luciole::ViewMode::Workbench));
    std::vector<std::size_t> rebound;
    int frames = 0;
    luciole::WorkbenchFolderSizes sizes(
        adapter, {.rebind = [&](std::size_t index) { rebound.push_back(index); }, .frameNeeded = [&] { ++frames; }}
    );
    sizes.mount(*session);
    const auto& model = rig.browser.model();
    const auto big = luciole::rowOf(model, rig.fixture.at("big"));
    TEST_CHECK(big.has_value());
    const auto bigIndex = static_cast<std::size_t>(std::ranges::find(model.rows(), *big) - model.rows().begin());
    auto tile = adapter.createTile();
    rig.frame(*session);
    adapter.bindTile(*tile, bigIndex, false, false);
    sizes.refresh();
    sizes.refresh();
    TEST_CHECK(rebound.empty() && frames == 0); // the measure has not given the row a text yet

    rig.settle(*session);
    sizes.refresh();
    TEST_CHECK(rebound == std::vector<std::size_t>{bigIndex} && frames == 1);
    adapter.bindTile(*tile, bigIndex, false, false);
    sizes.refresh();
    sizes.refresh();
    TEST_CHECK(rebound.size() == 1 && frames == 1); // the same measure binds nothing again

    sizes.unmount();
    sizes.refresh();
    TEST_CHECK(rebound.size() == 1 && frames == 1); // nothing is mounted
  }
} // namespace

int main() {
  refreshBindsOnlyStaleRows();
  return 0;
}
