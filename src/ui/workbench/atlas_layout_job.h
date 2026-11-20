#pragma once

#include "workbench/atlas_camera.h"
#include "workbench/atlas_scene.h"
#include "workbench/circle_pack.h"
#include "workbench/measure.h"
#include "workbench/pane_layout.h"
#include "workbench/tree.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The Atlas layout a worker runs: the scene for a camera, and the camera carried into a new snapshot's frame.
namespace luciole {
  // What a layout starts from.
  struct AtlasJob {
    std::shared_ptr<const workbench::Tree> tree;
    std::uint64_t serial = 0;
    workbench::LayoutSettings settings;
    workbench::ViewSize view;                   // the map's size
    bool adopt = false;                         // a new snapshot: the camera moves into its current folder's frame
    std::vector<std::string> names;             // with adopt, the camera's folder in the shown snapshot
    workbench::NodeId cwd = workbench::kNoNode; // without adopt, the camera's folder in `tree`
    workbench::Camera camera;
    std::uint64_t cwdMoves = 0; // the part's count of folder changes when the job started
    double margin = 0;          // share of each side the scene reaches beyond the view
  };

  // How the camera reached a new snapshot's frame.
  enum class AtlasArrival : std::uint8_t {
    Same, // the snapshot shows the folder the camera was in
    Near, // within two steps: the camera keeps the picture and can fly to rest
    Far,  // the camera starts at rest
  };

  struct AtlasBuilt {
    AtlasJob job;                                // with adopt, cwd and camera in the new frame
    std::optional<workbench::Similarity> change; // with adopt and a Same or Near arrival
    AtlasArrival arrival = AtlasArrival::Same;
    workbench::AtlasScene scene;
    std::unique_ptr<workbench::PackingCache> packings;
  };

  // Names from the top of the snapshot down to `id`.
  std::vector<std::string> atlasNamesTo(const workbench::Tree& tree, workbench::NodeId id);

  // Lays out `job` with `packings`, which the result hands back. Runs on a worker.
  AtlasBuilt buildAtlasLayout(AtlasJob job, std::unique_ptr<workbench::PackingCache> packings);
} // namespace luciole
