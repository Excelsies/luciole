#include "app/operation_hub.h"
#include "fs/gio_interop.h"
#include "operations/copy_receipt.h"
#include "test_check.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace {
  using namespace luciole;
  namespace fs = std::filesystem;
  std::optional<fs::path> backendRoot;
  std::optional<Location> providerRoot;
  bool crossFilesystem = false;
  bool upload = false, download = false;
  std::vector<TransferAction> actions() {
    return upload || download ? std::vector{TransferAction::Copy}
                              : std::vector{TransferAction::Copy, TransferAction::Move};
  }
  void write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path);
    stream << bytes;
    TEST_CHECK(stream.good());
  }
  std::string read(const fs::path& path) {
    std::ifstream stream(path);
    TEST_CHECK(stream.good());
    return {std::istreambuf_iterator<char>(stream), {}};
  }
  struct Fixture {
    fs::path root;
    fs::path crossDestination;
    Fixture() {
      const auto pattern = backendRoot ? (*backendRoot / "luciole-operation-history-XXXXXX").native()
                                       : std::string("luciole-operation-history-XXXXXX");
      const luciole::GString path(
          backendRoot ? g_mkdtemp(g_strdup(pattern.c_str())) : g_dir_make_tmp(pattern.c_str(), nullptr)
      );
      TEST_CHECK(path);
      root = path.get();
      fs::create_directories(root / "source");
      if (crossFilesystem) {
        const luciole::GString target(g_dir_make_tmp("luciole-history-cross-XXXXXX", nullptr));
        TEST_CHECK(target);
        crossDestination = target.get();
        // The private runner points TMPDIR at a different disposable filesystem.
        struct stat sourceFacts{}, destinationFacts{};
        TEST_CHECK(::stat(root.c_str(), &sourceFacts) == 0);
        TEST_CHECK(::stat(crossDestination.c_str(), &destinationFacts) == 0);
        TEST_CHECK(sourceFacts.st_dev != destinationFacts.st_dev);
        fs::create_directory_symlink(crossDestination, root / "destination");
      } else {
        fs::create_directories(root / "destination");
      }
      write(root / "canary", "unchanged outside data");
    }
    ~Fixture() {
      std::error_code ignored;
      fs::remove_all(root, ignored);
      if (!crossDestination.empty()) {
        fs::remove_all(crossDestination, ignored);
      }
    }
    Location at(const std::string& path) const {
      if (!providerRoot || (upload && path.starts_with("source")) || (download && path.starts_with("destination"))) {
        return Location::fromPath((root / path).native());
      }
      return providerRoot->child(root.filename().native()).resolve(path);
    }
  };
  void noHistory(const fs::path& root) {
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
      TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-"));
    }
    if (crossFilesystem && fs::is_symlink(root / "destination")) {
      noHistory(fs::read_symlink(root / "destination"));
    }
  }
  struct Owner final : OperationHub::Owner {
    void jobChanged(OperationJob&) override {}
    void transfersFinished(const std::vector<TransferResult>&) override {}
  };
  struct Session {
    TabWorkspace workspace;
    WindowId window = workspace.addWindow();
    TabId page = workspace.addTab(window);
    Owner owner;
    OperationHub hub{workspace};
    bool merge = true;
    Session() { hub.attach(page, owner); }
    void wait() {
      const auto deadline = g_get_monotonic_time() + 30000000;
      while (hub.busy()) {
        while (g_main_context_iteration(nullptr, false)) {
        }
        if (auto* job = hub.oldest(page, [](const auto& item) { return item.conflict().has_value(); })) {
          const auto conflict = job->conflict();
          TEST_CHECK(job->resolve(
              conflict->id,
              merge && conflict->canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::Replace
          ));
        }
        TEST_CHECK(g_get_monotonic_time() < deadline);
        g_usleep(1000);
      }
    }
    ~Session() {
      hub.shutdown();
      wait();
      hub.detach(page);
    }
    std::shared_ptr<OperationJob>
    transfer(std::vector<Location> sources, const Location& target, TransferAction action) {
      auto job = hub.create<TransferService>(page);
      TEST_CHECK(job->service<TransferService>().start({std::move(sources), target, action}));
      wait();
      for (const auto& result : job->service<TransferService>().results()) {
        if (result.error) {
          std::cerr << result.error->message << '\n';
        }
        TEST_CHECK(!result.error);
      }
      TEST_CHECK(hub.canUndo());
      return job;
    }
    OperationJob& replay(bool redo, bool success = true) {
      TEST_CHECK(hub.replayHistory(page, redo));
      wait();
      auto* job = hub.newest(page, [](const auto& item) { return item.kind() == JobKind::Transfer; });
      TEST_CHECK(job);
      const auto& results = job->service<TransferService>().results();
      TEST_CHECK(!results.empty());
      if (success) {
        for (const auto& result : results) {
          if (result.error) {
            std::cerr << (redo ? "Redo: " : "Undo: ") << job->service<TransferService>().report() << '\n';
          }
          TEST_CHECK(!result.error);
        }
        TEST_CHECK(redo ? hub.canUndo() : hub.canRedo());
      } else {
        TEST_CHECK(std::ranges::any_of(results, [](const auto& item) { return item.error.has_value(); }));
      }
      return *job;
    }
  };

  void newNames() {
    for (const auto action : actions()) {
      for (const bool folder : {false, true}) {
        Fixture fixture;
        const auto* tail = folder ? "item/nested/payload" : "item";
        write(fixture.root / "source" / tail, "incoming bytes");
        Session session;
        session.transfer({fixture.at("source/item")}, fixture.at("destination"), action);
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
          TEST_CHECK(read(fixture.root / "destination" / tail) == "incoming bytes");
          noHistory(fixture.root);
          const auto& undo = session.replay(false);
          if (action == TransferAction::Copy) {
            TEST_CHECK(undo.service<TransferService>().results().front().outcome == TransferOutcome::Deleted);
            TEST_CHECK(!undo.service<TransferService>().results().front().destination);
          }
          TEST_CHECK(!fs::exists(fixture.root / "destination/item"));
          TEST_CHECK(read(fixture.root / "source" / tail) == "incoming bytes");
          noHistory(fixture.root);
          session.replay(true);
        }
        session.hub.shutdown();
        session.wait();
        noHistory(fixture.root);
        TEST_CHECK(read(fixture.root / "canary") == "unchanged outside data");
      }
    }
  }

  void copyReadsCurrentSource() {
    Fixture fixture;
    write(fixture.root / "source/item", "first bytes");
    Session session;
    session.transfer({fixture.at("source/item")}, fixture.at("destination"), TransferAction::Copy);
    session.replay(false);
    write(fixture.root / "source/item", "current source bytes");
    session.replay(true);
    TEST_CHECK(read(fixture.root / "destination/item") == "current source bytes");
    session.replay(false);
    TEST_CHECK(fs::remove(fixture.root / "source/item"));
    session.replay(true, false);
    TEST_CHECK(session.hub.canRedo() && !fs::exists(fixture.root / "destination/item"));
    write(fixture.root / "source/item", "source available again");
    session.replay(true);
    TEST_CHECK(read(fixture.root / "destination/item") == "source available again");
    noHistory(fixture.root);
  }

  void linkWithoutSource() {
    Fixture fixture;
    write(fixture.root / "source/item", "original bytes");
    Session session;
    session.transfer({fixture.at("source/item")}, fixture.at("destination"), TransferAction::Link);
    session.replay(false);
    TEST_CHECK(fs::remove(fixture.root / "source/item"));
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
      session.replay(true);
      TEST_CHECK(fs::read_symlink(fixture.root / "destination/item") == fixture.root / "source/item");
      session.replay(false);
      noHistory(fixture.root);
    }
  }

  void cancelledPreflight() {
    Fixture fixture;
    write(fixture.root / "source/item", "unchanged bytes");
    TransferService service;
    service.useOperationHistory();
    TEST_CHECK(service.start({{fixture.at("source/item")}, fixture.at("destination"), TransferAction::Copy}));
    const auto deadline = g_get_monotonic_time() + 30000000;
    while (service.busy()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
    const auto receipt = service.copyReceipt();
    TEST_CHECK(receipt);
    CancelToken cancelled;
    cancelled.cancel();
    std::atomic<std::size_t> completed{0};
    const auto refused = receipt->reverse(cancelled, completed);
    TEST_CHECK(!refused.retireHistory && completed == 0 && !refused.results.empty());
    TEST_CHECK(refused.results.front().outcome == TransferOutcome::Cancelled);
    TEST_CHECK(read(fixture.root / "destination/item") == "unchanged bytes");
    const auto accepted = receipt->reverse(CancelToken{}, completed);
    TEST_CHECK(accepted.receipt && completed == 1);
    TEST_CHECK(accepted.results.front().outcome == TransferOutcome::Deleted);
    TEST_CHECK(!fs::exists(fixture.root / "destination/item"));
    noHistory(fixture.root);
  }

  void largeNativeMove() {
    Fixture fixture;
    fs::create_directory(fixture.root / "source/tree");
    for (unsigned item = 0; item < 16400; ++item) {
      write(fixture.root / "source/tree" / std::to_string(item), "payload");
    }
    Session session;
    session.transfer({fixture.at("source/tree")}, fixture.at("destination"), TransferAction::Move);
    session.replay(false);
    session.replay(true);
    TEST_CHECK(read(fixture.root / "destination/tree/16399") == "payload");
    noHistory(fixture.root);
  }

  void fullSelectionPreflight() {
    for (const auto action : actions()) {
      Fixture fixture;
      write(fixture.root / "source/a", "a bytes");
      write(fixture.root / "source/b", "b bytes");
      Session session;
      session.transfer({fixture.at("source/a"), fixture.at("source/b")}, fixture.at("destination"), action);
      const bool redo = action == TransferAction::Copy;
      if (redo) {
        session.replay(false);
      }
      const auto blocked = fixture.root / (redo ? "destination/b" : "source/b");
      write(blocked, "unrelated occupant");
      session.replay(redo, false);
      TEST_CHECK(read(blocked) == "unrelated occupant");
      TEST_CHECK(!fs::exists(fixture.root / (redo ? "destination/a" : "source/a")));
      TEST_CHECK(redo ? session.hub.canRedo() : session.hub.canUndo());
      TEST_CHECK(fs::remove(blocked));
      session.replay(redo);
      noHistory(fixture.root);
    }
  }

  void changedOutputAndParent() {
    for (const bool parent : {false, true}) {
      Fixture fixture;
      write(fixture.root / "source/item", "original bytes");
      Session session;
      session.transfer({fixture.at("source/item")}, fixture.at("destination"), TransferAction::Copy);
      if (parent) {
        fs::rename(fixture.root / "destination", fixture.root / "original-destination");
        fs::create_directory(fixture.root / "destination");
        write(fixture.root / "destination/item", "foreign parent bytes");
      } else {
        write(fixture.root / "destination/item", "externally changed bytes");
      }
      session.replay(false, false);
      TEST_CHECK(!session.hub.canUndo() && !session.hub.canRedo());
      TEST_CHECK(
          read(fixture.root / "destination/item") == (parent ? "foreign parent bytes" : "externally changed bytes")
      );
      TEST_CHECK(read(fixture.root / "source/item") == "original bytes");
      noHistory(fixture.root);
    }
  }

  void merges() {
    for (const auto action : actions()) {
      for (const bool empty : {false, true}) {
        if (empty && action == TransferAction::Copy) {
          continue;
        }
        Fixture fixture;
        fs::create_directories(fixture.root / "source/tree/nested");
        fs::create_directories(fixture.root / "destination/tree/nested");
        write(fixture.root / "destination/tree/keep", "destination-only bytes");
        if (!empty) {
          write(fixture.root / "source/tree/nested/item", "merged bytes");
          write(fixture.root / "destination/tree/nested/item", "overwritten bytes");
        }
        TEST_CHECK(::chmod((fixture.root / "source/tree").c_str(), 0710) == 0);
        std::vector<char> originalAcl(1024);
        if (!providerRoot) {
          TEST_CHECK(
              ::setxattr((fixture.root / "source/tree").c_str(), "user.luciole-test", "folder metadata", 15, 0) == 0
          );
          const auto aclText = "u::rwx,u:" + std::to_string(::geteuid()) + ":r-x,g::r-x,m::r-x,o::---";
          auto* acl = ::acl_from_text(aclText.c_str());
          TEST_CHECK(acl);
          TEST_CHECK(::acl_set_file((fixture.root / "source/tree").c_str(), ACL_TYPE_DEFAULT, acl) == 0);
          TEST_CHECK(::acl_free(acl) == 0);
          const auto size = ::getxattr(
              (fixture.root / "source/tree").c_str(), "system.posix_acl_default", originalAcl.data(), originalAcl.size()
          );
          TEST_CHECK(size > 0);
          originalAcl.resize(static_cast<std::size_t>(size));
        }
        Session session;
        session.transfer({fixture.at("source/tree")}, fixture.at("destination"), action);
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
          noHistory(fixture.root);
          session.replay(false);
          TEST_CHECK(read(fixture.root / "destination/tree/keep") == "destination-only bytes");
          TEST_CHECK(!fs::exists(fixture.root / "destination/tree/nested/item"));
          if (!empty) {
            TEST_CHECK(read(fixture.root / "source/tree/nested/item") == "merged bytes");
          }
          struct stat facts{};
          TEST_CHECK(::stat((fixture.root / "source/tree").c_str(), &facts) == 0);
          TEST_CHECK((facts.st_mode & 0777U) == 0710);
          if (!providerRoot) {
            char attribute[64]{};
            TEST_CHECK(
                ::getxattr((fixture.root / "source/tree").c_str(), "user.luciole-test", attribute, sizeof(attribute))
                == 15
            );
            TEST_CHECK(std::string(attribute, 15) == "folder metadata");
            std::vector<char> restoredAcl(1024);
            const auto size = ::getxattr(
                (fixture.root / "source/tree").c_str(), "system.posix_acl_default", restoredAcl.data(),
                restoredAcl.size()
            );
            TEST_CHECK(size > 0);
            restoredAcl.resize(static_cast<std::size_t>(size));
            TEST_CHECK(restoredAcl == originalAcl);
          }
          noHistory(fixture.root);
          session.replay(true);
          TEST_CHECK(read(fixture.root / "destination/tree/keep") == "destination-only bytes");
          TEST_CHECK(fs::exists(fixture.root / "source/tree") == (action == TransferAction::Copy));
        }
      }
    }
  }

  void replacements() {
    for (const auto action : actions()) {
      for (const bool folder : {false, true}) {
        Fixture fixture;
        const auto* tail = folder ? "item/payload" : "item";
        write(fixture.root / "source" / tail, "replacement bytes");
        write(fixture.root / "destination" / tail, "discarded previous bytes");
        Session session;
        session.merge = false;
        session.transfer({fixture.at("source/item")}, fixture.at("destination"), action);
        TEST_CHECK(read(fixture.root / "destination" / tail) == "replacement bytes");
        noHistory(fixture.root);
        session.replay(false);
        TEST_CHECK(!fs::exists(fixture.root / "destination/item"));
        TEST_CHECK(read(fixture.root / "source" / tail) == "replacement bytes");
        noHistory(fixture.root);
        session.replay(true);
        TEST_CHECK(read(fixture.root / "destination" / tail) == "replacement bytes");
        noHistory(fixture.root);
      }
    }
  }
} // namespace

int main(int argc, char** argv) {
  try {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (argc == 4 && (mode == "--provider" || mode == "--upload" || mode == "--download")) {
      backendRoot = argv[2];
      providerRoot = Location::fromUri(argv[3]);
      upload = mode == "--upload";
      download = mode == "--download";
    } else if (argc == 3 && std::string_view(argv[1]) == "--cross-filesystem") {
      backendRoot = argv[2];
      crossFilesystem = true;
    } else {
      TEST_CHECK(argc == 1);
    }
    newNames();
    copyReadsCurrentSource();
    if (!upload && !download) {
      linkWithoutSource();
    }
    cancelledPreflight();
    if (!providerRoot && !crossFilesystem) {
      largeNativeMove();
    }
    fullSelectionPreflight();
    changedOutputAndParent();
    merges();
    replacements();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "operation history: " << error.what() << '\n';
    return 1;
  }
}
