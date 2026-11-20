#include "browser/workbench_model.h"
#include "browser/workbench_services.h"
#include "event_loop/glib_poll_source.h"
#include "temp_folder.h"
#include "test_check.h"
#include "workbench/format.h"
#include "workbench/measure.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <poll.h>
#include <print>
#include <string>
#include <vector>

// Going up above the folder a session started in, through folders the ancestor budget cut short: every folder the
// snapshots show reads the true totals, or says it is still measuring, or says it is a lower bound; never a zero or a
// lower bound passed off as exact. A cut-short ancestor stays a lower bound, not "measuring…", and the true totals
// arrive once the folder is opened.
namespace {
  namespace fs = std::filesystem;
  using luciole::Location;
  using luciole::WorkbenchMeasure;
  using luciole::WorkbenchModel;
  using luciole::workbench::measureLabel;
  using luciole::workbench::NodeId;
  using Snapshot = std::shared_ptr<const luciole::WorkbenchSnapshot>;

  constexpr std::size_t kBulkFiles = 300;
  constexpr std::chrono::seconds kDeadline{10};

  Location at(const fs::path& path) { return Location::fromPath(path.native()); }

  void writeFile(const fs::path& path, std::size_t bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << std::string(bytes, 'x');
  }

  struct Truth {
    double bytes = 0;
    double items = 0;
  };

  // The true totals of each folder by name, counted with the standard library, independent of the measure service.
  std::map<std::string, Truth> truthOf(const fs::path& root) {
    std::map<std::string, Truth> truth;
    std::vector<fs::path> folders{root};
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
      if (entry.is_directory()) {
        folders.push_back(entry.path());
      }
    }
    for (const auto& path : folders) {
      double bytes = 0;
      for (const auto& file : fs::recursive_directory_iterator(path)) {
        bytes += file.is_regular_file() ? static_cast<double>(file.file_size()) : 0;
      }
      const auto items = static_cast<double>(std::ranges::distance(fs::directory_iterator(path)));
      truth[path.filename().string()] = {.bytes = bytes, .items = items};
    }
    return truth;
  }

  // A folder the snapshot shows tells the true size and count, or says it is measuring or a lower bound, and then
  // never says zero as a fact.
  void checkNode(const Snapshot& snapshot, NodeId id, const std::map<std::string, Truth>& truth) {
    const auto& tree = snapshot->tree;
    const auto& node = tree.nodes[id];
    const auto found = truth.find(node.name);
    if (!node.folder || found == truth.end()) {
      return;
    }
    const auto bytes = measureLabel(tree, id, WorkbenchMeasure::Bytes);
    const auto items = measureLabel(tree, id, WorkbenchMeasure::Items);
    const auto trueBytes = luciole::workbench::sizeText(found->second.bytes);
    const auto trueItems = luciole::workbench::itemsText(found->second.items);
    if (bytes != trueBytes) {
      TEST_CHECK(node.partial || node.lowerBound);
      TEST_CHECK(bytes != "0 bytes");
      TEST_CHECK(node.partial || bytes == "not measured" || bytes.starts_with("at least "));
    }
    if (items != trueItems) {
      TEST_CHECK(node.partial || node.lowerBound);
      TEST_CHECK(items != "0 items");
    }
  }

  void checkSnapshot(const Snapshot& snapshot, const std::map<std::string, Truth>& truth) {
    const auto& tree = snapshot->tree;
    checkNode(snapshot, tree.root, truth);
    for (const auto id : luciole::workbench::children(tree, tree.root)) {
      checkNode(snapshot, id, truth);
    }
    for (const auto id : tree.ancestors) {
      checkNode(snapshot, id, truth);
    }
  }

  // The ancestor of the snapshot's tree of this name, or null.
  const luciole::workbench::Node* ancestorNamed(const luciole::workbench::Tree& tree, const std::string& name) {
    for (const auto id : tree.ancestors) {
      if (tree.nodes[id].name == name) {
        return &tree.nodes[id];
      }
    }
    return nullptr;
  }
} // namespace

int main() {
  luciole::test::TempFolder folder{"luciole-ancestor-measure"};
  const auto& base = folder.path();
  std::vector<fs::path> chain{base};
  for (const char* name : {"a1", "a2", "a3", "a4"}) {
    chain.push_back(chain.back() / name);
  }
  for (std::size_t i = 0; i < chain.size(); ++i) {
    for (int f = 0; f < 3; ++f) {
      writeFile(chain[i] / std::format("f{}-{}", i, f), 1000 * (i + 1));
    }
  }
  for (std::size_t f = 0; f < kBulkFiles; ++f) {
    writeFile(base / "aaa_bulk" / std::format("b{}", f), 10);
  }
  const auto truth = truthOf(base);

  // Ancestors are measured under a budget far below the bulk folder, one walk at a time.
  luciole::MeasureServiceOptions options;
  options.ancestorBudget = 50;
  options.workers = 1;
  luciole::SubtreeMeasureService service(options);
  luciole::MeasureCache cache(service);
  WorkbenchModel model(cache);
  GlibPollSource source;
  const auto until = [&](const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    while (!done()) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::vector<pollfd> fds;
      const auto start = source.addPollFds(fds);
      TEST_CHECK(::poll(fds.data(), fds.size(), 5) >= 0);
      source.dispatch(fds, start);
    }
  };
  Snapshot last;
  model.setChangedCallback([&] {
    if (const auto s = model.snapshot(); s && s != last) {
      last = s;
      checkSnapshot(s, truth);
    }
  });
  // The session starts at a3: the ancestors above it are measured under the budget and the bulk folder at the top
  // exceeds it.
  const auto topName = base.filename().string();
  model.setLocation(at(chain[3]));
  until([&] {
    const auto shown = model.snapshot();
    const auto* node = shown && shown->location == at(chain[3]) ? ancestorNamed(shown->tree, topName) : nullptr;
    return node != nullptr && !node->partial && !model.measuring();
  });
  const auto shown = model.snapshot();
  const auto& below = shown->tree;
  const auto* cut = ancestorNamed(below, topName);
  TEST_CHECK(cut != nullptr && cut->lowerBound && !cut->partial);
  const auto bytes = measureLabel(below, static_cast<NodeId>(cut - below.nodes.data()), WorkbenchMeasure::Bytes);
  TEST_CHECK(bytes.starts_with("at least ") && bytes != "measuring…");
  for (const char* name : {"a1", "a2"}) {
    const auto* whole = ancestorNamed(below, name);
    TEST_CHECK(whole != nullptr && !whole->lowerBound && !whole->partial && whole->bytes == truth.at(name).bytes);
  }
  // Then up to the top, which is measured in full.
  for (const auto index : {2U, 1U, 0U}) {
    model.setLocation(at(chain[index]));
    until([&] { return model.snapshot() && model.snapshot()->location == at(chain[index]); });
  }
  until([&] { return !model.measuring(); });
  const auto& top = model.snapshot()->tree;
  TEST_CHECK(measureLabel(top, top.root, WorkbenchMeasure::Items) == "5 items");
  TEST_CHECK(!top.nodes[top.root].partial && !top.nodes[top.root].lowerBound);
  const double expected = truth.at(base.filename().string()).bytes;
  TEST_CHECK(top.nodes[top.root].bytes == expected);
  TEST_CHECK(top.nodes[luciole::workbench::children(top, top.root)[0]].bytes > 0);
  return 0;
}
