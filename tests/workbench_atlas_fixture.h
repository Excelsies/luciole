#pragma once

#include "test_check.h"
#include "workbench/atlas_camera.h"
#include "workbench/circle_pack.h"
#include "workbench/measure.h"
#include "workbench/tree.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <string>
#include <vector>

namespace luciole::test::atlas {

  inline constexpr double kExact = 1e-9;
  inline constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  inline constexpr double kInfinity = std::numeric_limits<double>::infinity();

  // Ids: 0 top, 1 home, 2 etc, 3 docs, 4 music, 5 notes.txt, 6 sub, 7 .cache, 8..77 f00..f69 (in docs),
  // 78..82 s0..s4 (in sub), 83 song.ogg (in music), 84 void (an empty folder in sub).
  inline constexpr workbench::NodeId kTop = 0;
  inline constexpr workbench::NodeId kHome = 1;
  inline constexpr workbench::NodeId kEtc = 2;
  inline constexpr workbench::NodeId kDocs = 3;
  inline constexpr workbench::NodeId kMusic = 4;
  inline constexpr workbench::NodeId kNotes = 5;
  inline constexpr workbench::NodeId kSub = 6;
  inline constexpr workbench::NodeId kFirstFile = 8;
  inline constexpr workbench::NodeId kFirstS = 78;
  inline constexpr workbench::NodeId kSong = 83;
  inline constexpr workbench::NodeId kVoid = 84;

  inline workbench::Tree atlasTree() {
    std::vector<std::string> names;
    names.reserve(75);
    for (int i = 0; i < 70; ++i) {
      names.push_back(std::format("f{:02}", i));
    }
    for (int i = 0; i < 5; ++i) {
      names.push_back(std::format("s{}", i));
    }
    std::vector<WorkbenchRow> rows{
        {.parent = -1, .name = "top", .folder = true, .bytes = 9000},
        {.parent = 0, .name = "home", .folder = true, .bytes = 6000},
        {.parent = 0, .name = "etc", .folder = true, .bytes = 800},
        {.parent = 1, .name = "docs", .folder = true, .bytes = 3000},
        {.parent = 1, .name = "music", .folder = true, .bytes = 1500},
        {.parent = 1, .name = "notes.txt", .bytes = 200},
        {.parent = 3, .name = "sub", .folder = true, .bytes = 2000},
        {.parent = 3, .name = ".cache", .folder = true, .bytes = 5000},
    };
    for (int i = 0; i < 70; ++i) {
      rows.push_back({.parent = 3, .name = names[static_cast<std::size_t>(i)], .bytes = 1000.0 - 10.0 * i});
    }
    for (int i = 0; i < 5; ++i) {
      rows.push_back({.parent = 6, .name = names[static_cast<std::size_t>(70 + i)], .bytes = 500.0 - 50.0 * i});
    }
    rows.push_back({.parent = 4, .name = "song.ogg", .bytes = 900});
    rows.push_back({.parent = 6, .name = "void", .folder = true});
    return workbenchTree(rows, 0);
  }

  // A tree, default settings and an empty cache that outlive the source pointing at them.
  struct World {
    workbench::Tree tree = atlasTree();
    workbench::LayoutSettings settings;
    workbench::PackingCache packings;
    workbench::AtlasSource source{.tree = tree, .settings = settings, .packings = packings};
  };

  inline void checkCamera(const workbench::Camera& camera, double cx, double cy, double scale) {
    TEST_NEAR(camera.cx, cx, kExact);
    TEST_NEAR(camera.cy, cy, kExact);
    TEST_NEAR(camera.scale, scale, kExact * std::max(1.0, scale));
  }

} // namespace luciole::test::atlas
