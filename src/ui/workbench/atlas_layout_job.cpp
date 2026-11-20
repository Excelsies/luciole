#include "ui/workbench/atlas_layout_job.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace luciole {
  namespace {
    using workbench::NodeId;

    // The node at the end of `names` from the top of the snapshot; kNoNode when the path leaves it.
    NodeId nodeAt(const workbench::Tree& tree, const std::vector<std::string>& names) {
      NodeId node = tree.ancestors.empty() ? tree.root : tree.ancestors.back();
      if (names.empty() || !workbench::hasNode(tree, node) || tree.nodes[node].name != names.front()) {
        return workbench::kNoNode;
      }
      for (std::size_t level = 1; level < names.size(); ++level) {
        const auto kids = workbench::children(tree, node);
        const auto found = std::ranges::find_if(kids, [&](NodeId kid) {
          return workbench::hasNode(tree, kid) && tree.nodes[kid].name == names[level];
        });
        if (found == kids.end()) {
          return workbench::kNoNode;
        }
        node = *found;
      }
      return node;
    }

    struct Change {
      std::optional<workbench::Similarity> sim;
      AtlasArrival arrival = AtlasArrival::Far;
    };

    // The map from the frame of the folder at `names` into the snapshot's current folder; none when the camera
    // cannot follow, so it rests.
    Change frameChange(const workbench::AtlasSource& source, const std::vector<std::string>& names) {
      const auto& tree = source.tree;
      const NodeId from = names.empty() ? workbench::kNoNode : nodeAt(tree, names);
      if (from == tree.root) {
        return {.sim = workbench::Similarity{}, .arrival = AtlasArrival::Same};
      }
      if (!workbench::hasNode(tree, from) || !workbench::convertCamera(source, from, tree.root, {})) {
        return {};
      }
      auto sim = workbench::similarity(source, from, tree.root);
      return {.sim = sim, .arrival = sim ? AtlasArrival::Near : AtlasArrival::Far};
    }
  } // namespace

  std::vector<std::string> atlasNamesTo(const workbench::Tree& tree, NodeId id) {
    std::vector<std::string> names;
    for (const auto node : workbench::pathFromTop(tree, id)) {
      names.push_back(tree.nodes[node].name);
    }
    return names;
  }

  AtlasBuilt buildAtlasLayout(AtlasJob job, std::unique_ptr<workbench::PackingCache> packings) {
    const workbench::AtlasSource source{.tree = *job.tree, .settings = job.settings, .packings = *packings};
    Change change;
    if (job.adopt) {
      change = frameChange(source, job.names);
      const auto camera = change.sim ? workbench::convert(job.camera, *change.sim) : workbench::Camera{};
      if (!workbench::usableCamera(camera)) {
        change = {};
      }
      job.camera = change.sim ? camera : workbench::restCamera(job.view);
      job.cwd = job.tree->root;
    }
    auto scene = workbench::buildSceneAround(source, job.cwd, job.camera, job.view, job.margin);
    return {
        .job = std::move(job),
        .change = change.sim,
        .arrival = change.arrival,
        .scene = std::move(scene),
        .packings = std::move(packings),
    };
  }
} // namespace luciole
