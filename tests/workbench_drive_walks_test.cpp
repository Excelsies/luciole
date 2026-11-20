#include "browser/workbench_services.h"
#include "fs/location_within.h"
#include "temp_folder.h"
#include "test_check.h"
#include "workbench_walks_rig.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

// A drive removed under a Workbench page: its walks leave the page, and the service reports the cancelled walk as
// reading until its thread has let go of the folders it holds, then nothing holds the drive.
namespace {
  using namespace luciole::test::walks;
  using luciole::MeasurePriority;
  using luciole::SubtreeMeasureService;

  // Holds a walk after its first partial table, so the walk certainly reads while the test looks.
  struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool reached = false;
    bool open = false;
    void hold() {
      std::unique_lock lock(mutex);
      if (std::exchange(reached, true)) {
        return;
      }
      changed.notify_all();
      TEST_CHECK(changed.wait_for(lock, std::chrono::seconds(20), [this] { return open; }));
    }
    // Waits until a walk is held.
    void awaitReached() {
      std::unique_lock lock(mutex);
      TEST_CHECK(changed.wait_for(lock, std::chrono::seconds(20), [this] { return reached; }));
    }
    void release() {
      const std::scoped_lock lock(mutex);
      open = true;
      changed.notify_all();
    }
  };

  // The descriptors of this process that name `root` or a file below it.
  std::size_t held(const fs::path& root) {
    std::size_t count = 0;
    for (const auto& file : fs::directory_iterator("/proc/self/fd")) {
      std::error_code error;
      const auto target = fs::read_symlink(file.path(), error).native();
      count += !error && (target == root.native() || target.starts_with(root.native() + "/"));
    }
    return count;
  }

  // A drive removed under a Workbench page: its walks leave the page, and the service reports the cancelled walk as
  // reading until its thread has let go of the folders it holds, then nothing holds the drive.
  void releasingTheDriveEndsItsWalks(const fs::path& base) {
    const auto volume = tree(base / "volume", 6, 20);
    const auto other = tree(base / "elsewhere", 1, 1);
    Pump pump;
    Gate gate;
    auto options = quickPublication(1);
    options.publishInterval = std::chrono::milliseconds(0); // every finished folder publishes
    options.afterPartial = [&gate] { gate.hold(); };
    SubtreeMeasureService service(options);
    MeasureCache cache(service);
    Driven page(cache);
    page.enter(volume);
    gate.awaitReached();
    TEST_CHECK(service.reading(volume));
    TEST_CHECK(held(base / "volume") > 0);
    TEST_CHECK(!service.reading(other));
    page.walks.release(other);
    TEST_CHECK(page.walks.walks().size() == 1);
    page.walks.release(volume);
    TEST_CHECK(page.walks.walks().empty());
    TEST_CHECK(service.reading(volume) && held(base / "volume") > 0); // the cancelled walk has not let go yet
    gate.release();
    pump.until([&] { return !service.reading(volume); });
    TEST_CHECK(held(base / "volume") == 0);
  }

} // namespace

int main() {
  luciole::test::TempFolder folder{"luciole-drive-walks"};
  releasingTheDriveEndsItsWalks(folder.path());
  return 0;
}
