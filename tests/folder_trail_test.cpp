#include "test_check.h"
#include "ui/workbench/workbench_columns.h"

#include <cstddef>
#include <format>
#include <print>
#include <string>
#include <vector>

namespace {
  using luciole::FolderTrail;
  using luciole::Location;
  using Locations = std::vector<Location>;

  Location at(const std::string& path) { return Location::fromPath(path); }

  void aVisitRemembersTheWayDown() {
    FolderTrail trail;
    TEST_CHECK(trail.trail(at("/"), 3).empty() && trail.serial() == 0);
    trail.visit(at("/a/b/c"));
    TEST_CHECK((trail.trail(at("/"), 3) == Locations{at("/a"), at("/a/b"), at("/a/b/c")}));
    TEST_CHECK((trail.trail(at("/a"), 3) == Locations{at("/a/b"), at("/a/b/c")}));
    TEST_CHECK((trail.trail(at("/"), 1) == Locations{at("/a")}));
    TEST_CHECK(trail.trail(at("/a/b/c"), 3).empty() && trail.trail(at("/x"), 3).empty());
    TEST_CHECK(trail.trail(at("/"), 0).empty());
    TEST_CHECK(trail.size() == 3);
    // going up keeps the way down; going elsewhere replaces it from where it parts
    const auto serial = trail.serial();
    trail.visit(at("/a"));
    TEST_CHECK(trail.serial() == serial);
    TEST_CHECK((trail.trail(at("/"), 3) == Locations{at("/a"), at("/a/b"), at("/a/b/c")}));
    trail.visit(at("/a/x"));
    TEST_CHECK(trail.serial() > serial);
    TEST_CHECK((trail.trail(at("/"), 3) == Locations{at("/a"), at("/a/x")}));
    TEST_CHECK((trail.trail(at("/a/b"), 3) == Locations{at("/a/b/c")})); // a folder left aside keeps its own
  }

  // Each visit of /f<i>/leaf remembers /f<i> and the root's child; the oldest folders go first.
  void keepsAtMostItsBound() {
    FolderTrail trail;
    const std::size_t visits = FolderTrail::kFolders + 50;
    for (std::size_t i = 0; i < visits; ++i) {
      trail.visit(at(std::format("/f{}/leaf", i)));
      TEST_CHECK(trail.size() <= FolderTrail::kFolders);
    }
    TEST_CHECK(trail.size() == FolderTrail::kFolders);
    const auto last = std::format("/f{}", visits - 1);
    TEST_CHECK((trail.trail(at("/"), 2) == Locations{at(last), at(last + "/leaf")}));
    TEST_CHECK(trail.trail(at("/f0"), 1).empty());
    TEST_CHECK(
        (trail.trail(at(std::format("/f{}", visits - 200)), 1) == Locations{at(std::format("/f{}/leaf", visits - 200))})
    );
  }

  // A visit records at most kDepth ancestors.
  void deepFoldersRecordABoundedWay() {
    std::string path;
    for (std::size_t level = 0; level < FolderTrail::kDepth + 10; ++level) {
      path += std::format("/d{}", level);
    }
    FolderTrail trail;
    trail.visit(at(path));
    TEST_CHECK(trail.size() == FolderTrail::kDepth);
    TEST_CHECK(trail.trail(at("/"), 1).empty()); // the root is beyond the bound
    TEST_CHECK(trail.trail(at(path.substr(0, path.rfind('/'))), 1).size() == 1);
  }
} // namespace

int main() {
  aVisitRemembersTheWayDown();
  keepsAtMostItsBound();
  deepFoldersRecordABoundedWay();
  std::println("folder trail checks passed");
}
