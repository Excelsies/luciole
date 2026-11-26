#include "operations/operation_history.h"

#include "fs/background_job.h"
#include "fs/gio_interop.h"
#include "fs/posix_fd.h"
#include "operations/copy_receipt.h"
#include "operations/creation_stage.h"
#include "operations/history_mount.h"
#include "operations/history_revisions.h"
#include "operations/move_receipt.h"
#include "operations/provider_copy_proof.h"
#include "operations/transfer_staging.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <ranges>
#include <set>
#include <sys/xattr.h>
#include <thread>
#include <variant>

namespace {
  using namespace luciole;
  namespace ph = provider_history;
  std::atomic<unsigned> pendingCleanup{0};

  bool inside(const Location& child, const Location& parent) {
    return transferLocationsEqual(child, parent) || transferLocationInside(child, parent);
  }
  std::string relative(const Location& child, const Location& parent) {
    std::vector<std::string> names;
    auto current = child;
    while (current != parent) {
      if (names.size() == 64 || !current.parent() || !current.basename()) {
        throwFileError(FileErrorKind::Unsupported, "History paths exceed the supported nesting depth.");
      }
      names.push_back(*current.basename());
      current = *current.parent();
    }
    std::string result;
    for (const auto& name : std::views::reverse(names)) {
      if (!result.empty()) {
        result += '/';
      }
      result += name;
    }
    return result;
  }
  Location relocated(const Location& value, const Location& from, const Location& to) {
    return value == from ? to : to.resolve(relative(value, from));
  }

  struct Proof {
    std::string name;
    std::variant<struct stat, ph::Tree> facts;
    std::vector<Proof> children;
    bool recursive = true;
    bool directory() const {
      if (const auto* native = std::get_if<struct stat>(&facts)) {
        return S_ISDIR(native->st_mode);
      }
      return std::get<ph::Tree>(facts).directory;
    }
  };
  Proof nativeProof(
      int parent, const std::string& name, std::size_t& remaining, unsigned depth, const CancelToken& token,
      bool recursive = true
  ) {
    ph::check(token);
    if (!remaining || depth > 64) {
      throwFileError(FileErrorKind::Unsupported, "Operation history exceeds its tree limit.");
    }
    --remaining;
    const auto facts = transfer::info(parent, name);
    Proof result{name, facts, {}};
    result.recursive = !S_ISDIR(facts.st_mode) || recursive;
    if (S_ISDIR(facts.st_mode) && recursive) {
      auto fd = transfer::directory(parent, name);
      transfer::requireUnchanged(facts, statFd(fd.get()));
      for (const auto& child : transfer::children(fd.get(), remaining + 1)) {
        result.children.push_back(nativeProof(fd.get(), child, remaining, depth + 1, token));
      }
      transfer::requireUnchanged(facts, statFd(fd.get()));
    } else if (!S_ISDIR(facts.st_mode) && !S_ISREG(facts.st_mode) && !S_ISLNK(facts.st_mode)) {
      throwFileError(FileErrorKind::Unsupported, "Operation history does not support special files.");
    }
    return result;
  }
  Proof remoteProof(ph::Tree tree, std::string name = {}) {
    Proof result{std::move(name), ph::Tree{}, {}};
    for (auto& child : tree.children) {
      auto childName = child.name;
      result.children.push_back(remoteProof(std::move(child), std::move(childName)));
    }
    tree.children.clear();
    result.facts = std::move(tree);
    return result;
  }
  ph::Tree providerTree(const Proof& proof) {
    auto result = std::get<ph::Tree>(proof.facts);
    result.name = proof.name;
    for (const auto& child : proof.children) {
      result.children.push_back(providerTree(child));
    }
    return result;
  }
  transfer::Tree nativeTree(const Proof& proof, const HistoryRevisions& revisions) {
    const auto& facts = std::get<struct stat>(proof.facts);
    transfer::Tree result{proof.name, S_ISDIR(facts.st_mode) ? facts : revisions.expected(facts), {}};
    for (const auto& child : proof.children) {
      result.children.push_back(nativeTree(child, revisions));
    }
    return result;
  }
  void remember(const Proof& proof, HistoryRevisions& revisions) {
    if (const auto* facts = std::get_if<struct stat>(&proof.facts); facts && !S_ISDIR(facts->st_mode)) {
      revisions.capture(*facts);
    }
    for (const auto& child : proof.children) {
      remember(child, revisions);
    }
  }
  void verify(
      const Location& location, const Proof& proof, const CancelToken& token,
      const std::shared_ptr<HistoryRevisions>& revisions
  ) {
    if (location.isNative()) {
      auto parent = checkedFd(::open(location.parent()->nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      std::atomic<std::uint64_t> bytes{0};
      transfer::CopyWork work(token, bytes);
      if (!proof.recursive) {
        transfer::requireUnchanged(
            std::get<struct stat>(proof.facts), transfer::info(parent.get(), *location.basename())
        );
        return;
      }
      auto tree = nativeTree(proof, *revisions);
      tree.name = *location.basename();
      transfer::verifyTree(parent.get(), tree, work);
    } else {
      auto tree = providerTree(proof);
      tree.name.clear();
      ph::verify(location, tree, token);
    }
  }
  void remove(
      const Location& location, const Proof& proof, const CancelToken& token,
      const std::shared_ptr<HistoryRevisions>& revisions
  ) {
    if (location.isNative()) {
      auto parent = checkedFd(::open(location.parent()->nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      std::atomic<std::uint64_t> bytes{0};
      transfer::CopyWork work(token, bytes);
      auto tree = nativeTree(proof, *revisions);
      work.history = std::make_shared<HistoryRevisions>();
      const auto seed = [&](auto&& self, const transfer::Tree& node) -> void {
        if (!S_ISDIR(node.metadata.st_mode)) {
          work.history->capture(node.metadata);
        }
        for (const auto& child : node.children) {
          self(self, child);
        }
      };
      seed(seed, tree);
      work.revisionObserver = [revisions](const struct stat& value, bool removed) {
        revisions->observed(value, removed);
      };
      tree.name = *location.basename();
      transfer::removeSource(parent.get(), tree, work);
    } else {
      auto tree = providerTree(proof);
      tree.name.clear();
      ph::remove(location, tree, token);
    }
  }
  struct Binding {
    Location location;
    Fd native;
    GioPtr<GFile> lease;
    std::map<std::string, std::string> attributes;
  };
  std::map<std::string, std::string> directoryAttributes(const Location& location, const CancelToken& token) {
    const auto value = ph::info(location, token);
    if (readIsSymlink(value.get()) || g_file_info_get_file_type(value.get()) != G_FILE_TYPE_DIRECTORY) {
      throwFileError(FileErrorKind::Changed, "An operation history containing folder changed.");
    }
    std::map<std::string, std::string> result;
    for (const auto* name : {"id::file", "id::filesystem", "unix::mode", "unix::uid", "unix::gid"}) {
      if (g_file_info_has_attribute(value.get(), name)) {
        const luciole::GString text(g_file_info_get_attribute_as_string(value.get(), name));
        result.emplace(name, text.get());
      }
    }
    return result;
  }
  void validate(const Binding& binding, const CancelToken& token) {
    ph::check(token);
    if (binding.native.valid()) {
      const auto current = checkedFd(::open(binding.location.nativePath()->c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
      if (!sameIdentity(statFd(current.get()), statFd(binding.native.get()))) {
        throwFileError(FileErrorKind::Changed, "An operation history containing folder was replaced.");
      }
    } else if (directoryAttributes(binding.location, token) != binding.attributes) {
      throwFileError(FileErrorKind::Changed, "An operation history containing folder changed.");
    }
  }
  struct HistoryDirectoryFacts {
    struct stat facts{};
    std::map<std::string, std::vector<char>> attributes;
  };
  std::shared_ptr<const HistoryDirectoryFacts> directoryMetadata(const Location& location) {
    if (!location.isNative()) {
      return {};
    }
    const auto fd = checkedFd(::open(location.nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    auto result = std::make_shared<HistoryDirectoryFacts>();
    result->facts = statFd(fd.get());
    const auto count = ::flistxattr(fd.get(), nullptr, 0);
    if (count < 0 && errno != ENOTSUP && errno != EOPNOTSUPP) {
      throwErrno(errno);
    }
    if (count > 1024 * 1024) {
      throwFileError(FileErrorKind::Unsupported, "Folder attributes exceed the supported history metadata size.");
    }
    if (count > 0) {
      std::vector<char> names(static_cast<std::size_t>(count));
      if (::flistxattr(fd.get(), names.data(), names.size()) != count) {
        throwFileError(FileErrorKind::Changed, "Folder attributes changed.");
      }
      for (std::size_t offset = 0; offset < names.size();) {
        const auto length = ::strnlen(names.data() + offset, names.size() - offset);
        if (length == names.size() - offset) {
          throwFileError(FileErrorKind::Changed, "Invalid folder attributes.");
        }
        const std::string name(names.data() + offset, length);
        offset += length + 1;
        if (!name.starts_with("user.") && name != "system.posix_acl_access" && name != "system.posix_acl_default") {
          continue;
        }
        const auto size = ::fgetxattr(fd.get(), name.c_str(), nullptr, 0);
        if (size < 0) {
          throwErrno(errno);
        }
        if (size > 1024 * 1024) {
          throwFileError(FileErrorKind::Unsupported, "A folder attribute exceeds the supported history metadata size.");
        }
        auto& value = result->attributes[name];
        value.resize(static_cast<std::size_t>(size));
        if (::fgetxattr(fd.get(), name.c_str(), value.data(), value.size()) != size) {
          throwFileError(FileErrorKind::Changed, "Folder attributes changed.");
        }
      }
    }
    transfer::requireUnchanged(result->facts, statFd(fd.get()));
    return result;
  }
  enum class EventKind { Transfer, RemovedDirectory, CreatedDirectory };
  struct Event {
    EventKind kind;
    Location source, destination;
    bool directory = false;
    unsigned mode = 0700;
    std::shared_ptr<const HistoryDirectoryFacts> metadata;
  };
  struct Owned {
    Location location;
    Proof proof;
  };
  void vacant(const Location& location, const CancelToken& token) {
    if (ph::info(location, token, true)) {
      throwFileError(FileErrorKind::Exists, "The history destination name is occupied: " + location.uri());
    }
  }
  bool complete(const TransferResult& result) {
    return !result.error
        && (result.outcome == TransferOutcome::Copied
            || result.outcome == TransferOutcome::Moved
            || result.outcome == TransferOutcome::Linked
            || result.outcome == TransferOutcome::Skipped)
        && std::ranges::all_of(result.children, complete);
  }
} // namespace

struct luciole::OperationHistory::Publication {
  Location source, destination;
  Proof proof;
};

struct luciole::OperationHistory::State {
  TransferAction action;
  CancelToken token;
  bool collecting = true, undone = false;
  std::vector<Event> events;
  std::vector<Owned> owned;
  std::vector<Binding> bindings;
  std::map<std::string, unsigned> mergeModes;
  std::map<std::string, std::shared_ptr<const HistoryDirectoryFacts>> mergeMetadata;
  std::vector<std::pair<Location, Location>> movedFolders;
  std::vector<Location> copyTargets;
  std::map<std::string, std::pair<Location, Location>> pendingSources;
  std::set<std::string> dirtyDirectories;
  std::shared_ptr<HistoryRevisions> revisions = std::make_shared<HistoryRevisions>();
  ph::Budget budget;
  std::mutex mutex;
  std::uint64_t generation = 0;

  explicit State(TransferAction value, CancelToken cancel) : action(value), token(std::move(cancel)) {}
  void disable() {
    collecting = false;
    events.clear();
    owned.clear();
    bindings.clear();
  }
  Location physical(Location location) const {
    for (const auto& [from, to] : movedFolders) {
      if (inside(location, from)) {
        location = relocated(location, from, to);
      }
    }
    return location;
  }
  void bind(const Location& location) {
    if (std::ranges::any_of(bindings, [&](const auto& value) { return value.location == location; })) {
      return;
    }
    Binding result{location, {}, {}, {}};
    if (location.isNative()) {
      result.native = checkedFd(::open(location.nativePath()->c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    } else {
      result.lease = gioFile(location);
      result.attributes = directoryAttributes(location, token);
    }
    bindings.push_back(std::move(result));
  }
  auto owner(const Location& location) {
    return std::ranges::find_if(owned, [&](const auto& value) { return inside(location, value.location); });
  }
  bool owns(const Location& location) const {
    return std::ranges::any_of(owned, [&](const auto& value) { return inside(location, value.location); });
  }
  void checkOwners(const Location& location) {
    if (auto found = owner(location); found != owned.end()) {
      verify(found->location, found->proof, token, revisions);
      if (!found->proof.recursive && found->location != location) {
        const auto parent =
            checkedFd(::open(found->location.parent()->nativePath()->c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
        found->proof = nativeProof(parent.get(), *found->location.basename(), budget.entries, 0, token);
        remember(found->proof, *revisions);
      }
    }
  }
  Proof* proofAt(const Location& location) {
    const auto found = owner(location);
    if (found == owned.end()) {
      return nullptr;
    }
    auto* result = &found->proof;
    if (found->location != location) {
      std::vector<std::string> names;
      auto current = location;
      while (current != found->location) {
        names.push_back(*current.basename());
        current = *current.parent();
      }
      for (const auto& name : std::views::reverse(names)) {
        const auto child = std::ranges::find(result->children, name, &Proof::name);
        if (child == result->children.end()) {
          return nullptr;
        }
        result = &*child;
      }
    }
    return result;
  }
  void edit(const Location& location, std::optional<Proof> value);
  void validateAll(const CancelToken& cancel) const;
  void finish();
  void refreshDirty();
  void restoreContainer(const Location& location, const Event& event);
  struct Reply {
    std::vector<TransferResult> results;
    std::shared_ptr<State> inverse;
    bool retire = false;
  };
  Reply reverse(const CancelToken& cancel, std::atomic<std::size_t>& completed, std::uint64_t version);
};

luciole::OperationHistory::OperationHistory(
    TransferAction action, CancelToken token, std::shared_ptr<HistoryRevisions> revisions
) {
  auto state = std::make_unique<State>(action, std::move(token));
  if (revisions) {
    state->revisions = std::move(revisions);
  }
  m_state = std::shared_ptr<State>(state.release(), [worker = std::this_thread::get_id()](State* retired) {
    auto owned = std::unique_ptr<State>(retired);
    if (worker == std::this_thread::get_id()) {
      return;
    }
    ++pendingCleanup;
    detail::runInThread([owned = std::move(owned)] mutable { owned.reset(); }, [] { --pendingCleanup; });
  });
}
luciole::OperationHistory::~OperationHistory() = default;
bool luciole::OperationHistory::cleanupPending() { return pendingCleanup != 0; }
bool luciole::OperationHistory::owns(const Location& destination) const { return m_state->owns(destination); }

std::vector<luciole::TransferResult>
luciole::OperationHistory::replayLink(const Location& source, const Location& destination) {
  TransferResult result{source, TransferOutcome::Failed};
  bool placed = false;
  try {
    const auto literal = transferLinkTarget(source, destination);
    if (!literal) {
      throwFileError(FileErrorKind::Unsupported, "The symbolic link namespace is unavailable.");
    }
    ph::check(m_state->token);
    beforeTransfer(source, destination);
    m_state->bind(*destination.parent());
    for (const auto& binding : m_state->bindings) {
      validate(binding, m_state->token);
    }
    if (destination.isNative()) {
      const auto parent =
          checkedFd(::open(destination.parent()->nativePath()->c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
      CreationStage stage(parent.get());
      if (::symlinkat(literal->c_str(), stage.fd(), "item") != 0) {
        throwErrno(errno);
      }
      stage.adopt(checkedFd(::openat(stage.fd(), "item", O_PATH | O_NOFOLLOW | O_CLOEXEC)));
      const auto publication = prepareNative(source, destination, stage.fd(), "item");
      ph::check(m_state->token);
      stage.publish(*destination.basename());
      placed = true;
      published(publication);
      if (!stage.clear()) {
        throwFileError(FileErrorKind::Other, "Could not remove symbolic link staging.");
      }
    } else {
      GError* error = nullptr;
      if (!g_file_make_symbolic_link(
              gioFile(destination).get(), literal->c_str(), gioCancellable(m_state->token), &error
          )) {
        const GErrorPtr owned(error);
        throw FileErrorException(fileError(*owned));
      }
      placed = true;
      const auto info = ph::info(destination, m_state->token);
      if (!readIsSymlink(info.get()) || g_strcmp0(g_file_info_get_symlink_target(info.get()), literal->c_str()) != 0) {
        throwFileError(FileErrorKind::Changed, "The replayed symbolic link changed.");
      }
      published(prepareProvider(source, destination, destination));
    }
    result.outcome = TransferOutcome::Linked;
    result.destination = destination;
  } catch (const std::exception&) {
    result.error = currentFileError();
    result.outcome = placed                              ? TransferOutcome::Partial
        : result.error->kind == FileErrorKind::Cancelled ? TransferOutcome::Cancelled
                                                         : TransferOutcome::Failed;
    if (placed) {
      result.destination = destination;
    }
  }
  return {std::move(result)};
}

std::shared_ptr<luciole::OperationHistory::Publication> luciole::OperationHistory::prepareNative(
    const Location& source, const Location& destination, int parent, const std::string& name, bool recursive
) {
  if (!m_state->collecting) {
    return {};
  }
  try {
    if (m_state->action == TransferAction::Move) {
      m_state->bind(*source.parent());
    }
    m_state->bind(*destination.parent());
    auto proof = nativeProof(parent, name, m_state->budget.entries, 0, m_state->token, recursive);
    // Keep revision facts without holding an open descriptor to the file payload.
    remember(proof, *m_state->revisions);
    return std::make_shared<Publication>(Publication{source, destination, std::move(proof)});
  } catch (const FileErrorException&) {
    m_state->disable();
    return {};
  }
}

std::shared_ptr<luciole::OperationHistory::Publication> luciole::OperationHistory::prepareProvider(
    const Location& source, const Location& destination, const Location& staged
) {
  if (!m_state->collecting) {
    return {};
  }
  try {
    if (m_state->action == TransferAction::Move) {
      m_state->bind(*source.parent());
    }
    m_state->bind(*destination.parent());
    Proof proof;
    if (destination.isNative()) {
      const auto parent = checkedFd(::open(staged.parent()->nativePath()->c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
      proof = nativeProof(parent.get(), *staged.basename(), m_state->budget.entries, 0, m_state->token);
      remember(proof, *m_state->revisions);
    } else {
      proof = remoteProof(ph::capture(staged, m_state->budget, m_state->token));
    }
    return std::make_shared<Publication>(Publication{source, destination, std::move(proof)});
  } catch (const FileErrorException&) {
    m_state->disable();
    return {};
  }
}

void luciole::OperationHistory::published(const std::shared_ptr<Publication>& publication, bool exchanged) {
  if (!publication || !m_state->collecting) {
    return;
  }
  try {
    auto proof = publication->proof;
    proof.name = *publication->destination.basename();
    if (auto* native = std::get_if<struct stat>(&proof.facts)) {
      auto parent =
          checkedFd(::open(publication->destination.parent()->nativePath()->c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
      const auto current = transfer::info(parent.get(), proof.name);
      if (S_ISDIR(native->st_mode)) {
        if (!sameRenamedRevision(*native, current, exchanged)) {
          throwFileError(FileErrorKind::Changed, "A published folder changed during its rename.");
        }
      } else {
        m_state->revisions->renamed(*native, current, exchanged);
      }
      *native = current;
    }
    verify(publication->destination, proof, m_state->token, m_state->revisions);
    if (m_state->action == TransferAction::Move) {
      const auto original = m_state->physical(publication->source);
      if (m_state->proofAt(publication->destination) || std::ranges::any_of(m_state->owned, [&](const auto& item) {
            return inside(item.location, publication->destination);
          })) {
        // A new move cannot overwrite an earlier moved source version and still restore every source.
        m_state->disable();
        return;
      }
      if (ph::info(original, m_state->token, true)) {
        m_state->pendingSources.insert_or_assign(
            publication->source.uri(), std::pair{original, publication->destination}
        );
      } else {
        m_state->edit(original, std::nullopt);
        if (proof.directory()) {
          m_state->movedFolders.emplace_back(original, publication->destination);
        }
      }
    }
    const bool directory = proof.directory();
    m_state->edit(publication->destination, std::move(proof));
    if (m_state->action != TransferAction::Move) {
      // Replacing a copied parent also discards the effects previously published below that name.
      std::erase_if(m_state->events, [&](const auto& event) {
        return inside(event.destination, publication->destination);
      });
    }
    m_state->events.push_back(
        {EventKind::Transfer, publication->source, publication->destination, directory, 0700, {}}
    );
    if (m_state->events.size() > 4096) {
      m_state->disable();
    }
  } catch (const FileErrorException&) {
    m_state->disable();
  }
}

void luciole::OperationHistory::beforeTransfer(const Location& source, const Location& destination) {
  if (!m_state->collecting) {
    return;
  }
  try {
    m_state->refreshDirty();
    if (m_state->action == TransferAction::Move && !transferLinkTarget(source, destination)) {
      m_state->disable();
      return;
    }
    m_state->checkOwners(destination);
    if (m_state->action == TransferAction::Move) {
      m_state->checkOwners(m_state->physical(source));
    }
  } catch (const FileErrorException&) {
    m_state->disable();
  }
}

void luciole::OperationHistory::observed(const struct stat& value, bool removed) {
  if (!m_state->collecting) {
    return;
  }
  try {
    m_state->revisions->observed(value, removed);
  } catch (const FileErrorException&) {
    m_state->disable();
  }
}

void luciole::OperationHistory::sourceRemoved(const Location& source) {
  if (!m_state->collecting) {
    return;
  }
  const auto found = m_state->pendingSources.find(source.uri());
  if (found == m_state->pendingSources.end()) {
    return;
  }
  try {
    const auto [original, destination] = found->second;
    const auto output = m_state->owner(destination);
    const bool directory =
        output != m_state->owned.end() && output->location == destination && output->proof.directory();
    m_state->edit(original, std::nullopt);
    if (directory) {
      m_state->movedFolders.emplace_back(original, destination);
    }
    m_state->pendingSources.erase(found);
  } catch (const FileErrorException&) {
    m_state->disable();
  }
}

void luciole::OperationHistory::beforeMerge(const Location& source) {
  if (!m_state->collecting || m_state->action != TransferAction::Move) {
    return;
  }
  try {
    const auto value = ph::info(source, m_state->token);
    const auto mode = g_file_info_has_attribute(value.get(), G_FILE_ATTRIBUTE_UNIX_MODE)
        ? g_file_info_get_attribute_uint32(value.get(), G_FILE_ATTRIBUTE_UNIX_MODE) & 07777U
        : 0700U;
    m_state->mergeModes.emplace(source.uri(), mode);
    m_state->mergeMetadata.emplace(source.uri(), directoryMetadata(source));
    m_state->bind(*source.parent());
  } catch (const FileErrorException&) {
    m_state->disable();
  }
}
void luciole::OperationHistory::removedMerge(const Location& source) {
  if (!m_state->collecting || m_state->action != TransferAction::Move) {
    return;
  }
  const auto mode = m_state->mergeModes.find(source.uri());
  if (mode == m_state->mergeModes.end()) {
    m_state->disable();
    return;
  }
  try {
    m_state->edit(m_state->physical(source), std::nullopt);
    m_state->events.push_back(
        {EventKind::RemovedDirectory, source, source, true, mode->second, m_state->mergeMetadata.at(source.uri())}
    );
  } catch (const FileErrorException&) {
    m_state->disable();
  }
}

namespace {
  void refreshDirectory(Proof& proof, const Location& location, const CancelToken& token) {
    if (auto* native = std::get_if<struct stat>(&proof.facts)) {
      struct stat current{};
      if (::lstat(location.nativePath()->c_str(), &current) != 0) {
        throwErrno(errno);
      }
      if (!sameIdentity(*native, current)
          || native->st_mode != current.st_mode
          || native->st_uid != current.st_uid
          || native->st_gid != current.st_gid) {
        throwFileError(FileErrorKind::Changed, "A history folder changed while its contents were updated.");
      }
      *native = current;
    } else {
      auto& facts = std::get<ph::Tree>(proof.facts);
      const auto current = ph::info(location, token);
      for (auto& [name, expected] : facts.attributes) {
        if (!g_file_info_has_attribute(current.get(), name.c_str())) {
          throwFileError(FileErrorKind::Changed, "Provider history metadata became unavailable.");
        }
        const luciole::GString text(g_file_info_get_attribute_as_string(current.get(), name.c_str()));
        const std::string actual = text.get();
        if (name == "standard::size" || name == "etag::value" || name.starts_with("time::modified")) {
          expected = actual;
        } else if (expected != actual) {
          throwFileError(FileErrorKind::Changed, "A provider history folder changed.");
        }
      }
    }
  }
} // namespace

void luciole::OperationHistory::State::edit(const Location& location, std::optional<Proof> value) {
  const auto found = owner(location);
  if (found == owned.end()) {
    if (value) {
      std::erase_if(owned, [&](const auto& item) { return inside(item.location, location); });
      owned.push_back({location, std::move(*value)});
    }
    return;
  }
  if (found->location == location) {
    if (value) {
      found->proof = std::move(*value);
    } else {
      owned.erase(found);
    }
    return;
  }
  std::vector<std::string> names;
  auto current = location;
  while (current != found->location) {
    names.push_back(*current.basename());
    current = *current.parent();
  }
  auto* proof = &found->proof;
  std::vector<std::pair<Proof*, Location>> ancestors{{proof, current}};
  for (const auto& name : std::views::reverse(names)) {
    auto child = std::ranges::find(proof->children, name, &Proof::name);
    if (current.child(name) == location) {
      if (child != proof->children.end()) {
        proof->children.erase(child);
      }
      if (value) {
        value->name = name;
        proof->children.push_back(std::move(*value));
        std::ranges::sort(proof->children, {}, &Proof::name);
      }
      break;
    }
    if (child == proof->children.end() || !child->directory()) {
      throwFileError(FileErrorKind::Changed, "A recorded operation traverses an unknown folder.");
    }
    current = current.child(name);
    proof = &*child;
    ancestors.emplace_back(proof, current);
  }
  for (auto& [ancestor, path] : ancestors) {
    refreshDirectory(*ancestor, path, token);
    dirtyDirectories.insert(path.uri());
  }
}

void luciole::OperationHistory::State::refreshDirty() {
  for (const auto& name : dirtyDirectories) {
    const auto path = Location::fromUri(name);
    auto found = owner(path);
    if (found == owned.end()) {
      continue;
    }
    auto* proof = &found->proof;
    std::vector<std::string> parts;
    auto current = path;
    while (current != found->location) {
      parts.push_back(*current.basename());
      current = *current.parent();
    }
    for (const auto& part : std::views::reverse(parts)) {
      auto child = std::ranges::find(proof->children, part, &Proof::name);
      if (child == proof->children.end()) {
        throwFileError(FileErrorKind::Changed, "A recorded folder disappeared during an operation.");
      }
      proof = &*child;
    }
    refreshDirectory(*proof, path, token);
  }
  dirtyDirectories.clear();
}

void luciole::OperationHistory::State::validateAll(const CancelToken& cancel) const {
  for (const auto& binding : bindings) {
    validate(binding, cancel);
  }
  for (const auto& item : owned) {
    verify(item.location, item.proof, cancel, revisions);
  }
}

void luciole::OperationHistory::State::finish() {
  if (!pendingSources.empty()) {
    throwFileError(FileErrorKind::Changed, "Some moved sources were not confirmed removed.");
  }
  refreshDirty();
  std::erase_if(bindings, [&](const auto& binding) {
    return std::ranges::any_of(events, [&](const auto& event) {
      return event.directory
          && ((action == TransferAction::Move && inside(binding.location, event.source))
              || (event.kind != EventKind::RemovedDirectory && inside(binding.location, event.destination)));
    });
  });
  validateAll(token);
  if (action != TransferAction::Move && !undone) {
    copyTargets.clear();
    for (const auto& item : owned) {
      copyTargets.push_back(item.location);
    }
  }
}

namespace luciole {
  class OperationCopyHistory final : public CopyReceipt {
  public:
    explicit OperationCopyHistory(std::shared_ptr<OperationHistory::State> state)
        : CopyReceipt(state->action), m_state(std::move(state)), m_generation(m_state->generation) {}
    CopyReplayResult reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const override {
      auto reply = m_state->reverse(token, completed, m_generation);
      return {
          std::move(reply.results), reply.inverse ? std::make_shared<OperationCopyHistory>(reply.inverse) : nullptr,
          reply.retire
      };
    }
    std::vector<TransferResult> unconfirmed(const FileError& error) const override {
      std::vector<TransferResult> result;
      for (const auto& event : m_state->events) {
        result.emplace_back(event.destination, TransferOutcome::Failed, error);
      }
      return result;
    }
    std::size_t size() const override { return m_state->events.size(); }
    bool usesMount(const HistoryMount& mount) const override {
      return std::ranges::any_of(
                 m_state->bindings,
                 [&](const auto& binding) {
                   return binding.native.valid() ? mount.contains(binding.native.get())
                                                 : mount.contains(binding.location);
                 }
             )
          || std::ranges::any_of(m_state->events, [&](const auto& event) {
               return mount.contains(event.destination)
                   || (m_state->undone && m_state->action == TransferAction::Copy && mount.contains(event.source));
             });
    }

  private:
    std::shared_ptr<OperationHistory::State> m_state;
    std::uint64_t m_generation;
  };

  class OperationMoveHistory final : public MoveReceipt {
  public:
    explicit OperationMoveHistory(std::shared_ptr<OperationHistory::State> state)
        : m_state(std::move(state)), m_generation(m_state->generation) {}
    MoveReplayResult reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const override {
      auto reply = m_state->reverse(token, completed, m_generation);
      return {
          std::move(reply.results), reply.inverse ? std::make_shared<OperationMoveHistory>(reply.inverse) : nullptr,
          reply.retire
      };
    }
    std::vector<TransferResult> unconfirmed(const FileError& error) const override {
      std::vector<TransferResult> result;
      for (const auto& event : m_state->events) {
        result.emplace_back(event.destination, TransferOutcome::Failed, error);
      }
      return result;
    }
    std::size_t size() const override { return m_state->events.size(); }
    bool usesMount(const HistoryMount& mount) const override {
      return std::ranges::any_of(
                 m_state->bindings,
                 [&](const auto& binding) {
                   return binding.native.valid() ? mount.contains(binding.native.get())
                                                 : mount.contains(binding.location);
                 }
             )
          || std::ranges::any_of(m_state->events, [&](const auto& event) {
               return mount.contains(event.source) || mount.contains(event.destination);
             });
    }

  private:
    std::shared_ptr<OperationHistory::State> m_state;
    std::uint64_t m_generation;
  };
} // namespace luciole

void luciole::OperationHistory::capture(
    const std::vector<TransferResult>& results, std::shared_ptr<const CopyReceipt>& copy,
    std::shared_ptr<const MoveReceipt>& move
) {
  if (!m_state->collecting || m_state->events.empty() || !std::ranges::all_of(results, complete)) {
    return;
  }
  try {
    m_state->finish();
    if (m_state->action == TransferAction::Move) {
      move = std::make_shared<OperationMoveHistory>(m_state);
    } else {
      copy = std::make_shared<OperationCopyHistory>(m_state);
    }
  } catch (const FileErrorException&) {
    m_state->disable();
  }
}

namespace {
  Binding duplicate(const Binding& binding) {
    return {
        binding.location, binding.native.valid() ? checkedFd(::fcntl(binding.native.get(), F_DUPFD_CLOEXEC, 0)) : Fd{},
        binding.lease ? GioPtr<GFile>(G_FILE(g_object_ref(binding.lease.get()))) : GioPtr<GFile>{}, binding.attributes
    };
  }
  void makeDirectory(const Location& location, const CancelToken& token) {
    ph::check(token);
    GError* error = nullptr;
    if (!g_file_make_directory(gioFile(location).get(), gioCancellable(token), &error)) {
      const GErrorPtr owned(error);
      throw FileErrorException(fileError(*owned));
    }
  }
  void setDirectoryMode(const Location& location, unsigned mode, const CancelToken& token) {
    if (location.isNative()) {
      const auto fd =
          checkedFd(::open(location.nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (::fchmod(fd.get(), mode) != 0) {
        throwErrno(errno);
      }
    } else {
      GError* error = nullptr;
      if (!g_file_set_attribute_uint32(
              gioFile(location).get(), G_FILE_ATTRIBUTE_UNIX_MODE, mode, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
              gioCancellable(token), &error
          )) {
        const GErrorPtr owned(error);
        throw FileErrorException(fileError(*owned));
      }
    }
  }
  void restoreHistoryDirectoryFacts(
      const Location& location, const HistoryDirectoryFacts& metadata, const struct stat& expected
  ) {
    const auto fd = checkedFd(::open(location.nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    const auto current = statFd(fd.get());
    transfer::requireUnchanged(expected, current);
    if ((current.st_uid != metadata.facts.st_uid || current.st_gid != metadata.facts.st_gid)
        && ::fchown(fd.get(), metadata.facts.st_uid, metadata.facts.st_gid) != 0) {
      throwErrno(errno);
    }
    if (::fchmod(fd.get(), metadata.facts.st_mode & 07777U) != 0) {
      throwErrno(errno);
    }
    // Newly created folders can inherit a different default ACL. Restore the recorded supported attributes.
    for (const auto* name : {"system.posix_acl_access", "system.posix_acl_default"}) {
      if (!metadata.attributes.contains(name)
          && ::fremovexattr(fd.get(), name) != 0
          && errno != ENODATA
          && errno != ENOTSUP
          && errno != EOPNOTSUPP) {
        throwErrno(errno);
      }
    }
    for (const auto& [name, value] : metadata.attributes) {
      if (::fsetxattr(fd.get(), name.c_str(), value.data(), value.size(), 0) != 0) {
        throwErrno(errno);
      }
    }
    const timespec times[]{metadata.facts.st_atim, metadata.facts.st_mtim};
    if (::futimens(fd.get(), times) != 0) {
      throwErrno(errno);
    }
    const auto restored = statFd(fd.get());
    if ((restored.st_mode & 07777U) != (metadata.facts.st_mode & 07777U)
        || restored.st_uid != metadata.facts.st_uid
        || restored.st_gid != metadata.facts.st_gid) {
      throwFileError(FileErrorKind::Changed, "The restored folder permissions or ownership could not be preserved.");
    }
  }
  bool exists(const Location& location, const CancelToken& token) { return ph::info(location, token, true) != nullptr; }
} // namespace

void luciole::OperationHistory::State::restoreContainer(const Location& location, const Event& event) {
  refreshDirty();
  checkOwners(location);
  auto* proof = proofAt(location);
  if (!proof) {
    throwFileError(FileErrorKind::Changed, "A restored source folder lost its history proof.");
  }
  if (event.metadata) {
    restoreHistoryDirectoryFacts(location, *event.metadata, std::get<struct stat>(proof->facts));
  } else {
    setDirectoryMode(location, event.mode, token);
  }
  if (auto* native = std::get_if<struct stat>(&proof->facts)) {
    struct stat current{};
    if (::lstat(location.nativePath()->c_str(), &current) != 0) {
      throwErrno(errno);
    }
    if (!sameIdentity(*native, current)) {
      throwFileError(FileErrorKind::Changed, "A restored source folder was replaced.");
    }
    *native = current;
  } else {
    auto& facts = std::get<ph::Tree>(proof->facts);
    if (facts.attributes.contains(G_FILE_ATTRIBUTE_UNIX_MODE)) {
      const auto current = ph::info(location, token);
      const luciole::GString text(g_file_info_get_attribute_as_string(current.get(), G_FILE_ATTRIBUTE_UNIX_MODE));
      facts.attributes[G_FILE_ATTRIBUTE_UNIX_MODE] = text.get();
    }
    refreshDirectory(*proof, location, token);
  }
}

luciole::OperationHistory::State::Reply luciole::OperationHistory::State::reverse(
    const CancelToken& cancel, std::atomic<std::size_t>& completed, std::uint64_t version
) {
  const std::unique_lock guard(mutex);
  Reply reply;
  bool applied = false;
  std::optional<Location> active;
  bool incompleteEffect = false;
  const bool copyUndo = action != TransferAction::Move && !undone;
  const bool copyRedo = action != TransferAction::Move && undone;
  try {
    ph::check(cancel);
    if (version != generation) {
      throwFileError(FileErrorKind::Changed, "This operation history record is stale.");
    }
    validateAll(cancel);
    OperationHistory next(action, cancel);
    next.m_state->revisions = revisions;
    for (const auto& binding : bindings) {
      next.m_state->bindings.push_back(duplicate(binding));
    }
    std::atomic<std::uint64_t> bytes{0};
    if (copyUndo) {
      for (const auto& item : std::views::reverse(owned)) {
        active = item.location;
        incompleteEffect = false;
        for (const auto& binding : bindings) {
          validate(binding, cancel);
        }
        verify(item.location, item.proof, cancel, revisions);
        reply.results.emplace_back(item.location, TransferOutcome::Failed);
        // Recursive deletion may complete some children before a later error or cancellation.
        applied = true;
        incompleteEffect = true;
        remove(item.location, item.proof, cancel, revisions);
        reply.results.back().outcome = TransferOutcome::Deleted;
        incompleteEffect = false;
        completed.fetch_add(1);
      }
      next.m_state->events = events;
      next.m_state->copyTargets = copyTargets;
      next.m_state->undone = true;
    } else if (copyRedo) {
      for (const auto& target : copyTargets) {
        vacant(target, cancel);
      }
      // Check every external source before publishing any replayed copy. Current source contents are read
      // by the ordinary transfer; history does not retain the previous bytes or require their old revision.
      for (std::size_t i = 0; i < events.size(); ++i) {
        const auto& event = events[i];
        const bool producedEarlier =
            std::any_of(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(i), [&](const auto& earlier) {
              return earlier.directory && inside(event.source, earlier.destination);
            });
        if (!producedEarlier && action != TransferAction::Link) {
          (void)ph::info(event.source, cancel);
        }
      }
      for (const auto& event : events) {
        active = event.source;
        ph::check(cancel);
        auto results = runHistoryTransfer(event.source, event.destination, action, next, cancel, bytes);
        applied |= std::ranges::any_of(results, [](const auto& result) {
          return result.outcome != TransferOutcome::Failed
              && result.outcome != TransferOutcome::Cancelled
              && result.outcome != TransferOutcome::Skipped;
        });
        reply.results.insert(reply.results.end(), results.begin(), results.end());
        if (!std::ranges::all_of(results, complete)) {
          throw FileErrorException(
              results.front().error.value_or(FileError{FileErrorKind::Other, "Copy replay did not complete."})
          );
        }
        completed.fetch_add(1);
      }
      if (!next.m_state->collecting) {
        throwFileError(FileErrorKind::Changed, "The replayed copies could not be verified for Undo.");
      }
      next.m_state->finish();
    } else {
      struct Task {
        Event event;
        Location from, to;
        bool done = false;
      };
      std::vector<Task> tasks;
      for (const auto& event : std::views::reverse(events)) {
        tasks.push_back({event, physical(event.destination), event.source});
      }
      // All occupied return names must belong to another recorded item that will move away first.
      for (const auto& task : tasks) {
        if (task.event.kind != EventKind::CreatedDirectory && exists(task.to, cancel)) {
          const bool vacated = std::ranges::any_of(tasks, [&](const auto& other) {
            return other.event.kind == EventKind::Transfer && inside(task.to, other.from);
          });
          if (!vacated) {
            throwFileError(FileErrorKind::Exists, "A Move history return name is occupied: " + task.to.uri());
          }
        }
      }
      next.m_state->owned = owned;
      std::vector<std::pair<Location, Event>> created;
      std::size_t remaining = tasks.size();
      while (remaining) {
        bool progressed = false;
        for (auto& task : tasks) {
          if (task.done) {
            continue;
          }
          ph::check(cancel);
          active = task.from;
          incompleteEffect = false;
          next.m_state->refreshDirty();
          if (task.event.kind == EventKind::CreatedDirectory) {
            const auto* proof = next.m_state->proofAt(task.from);
            if (!proof || !proof->children.empty()) {
              continue;
            }
            next.beforeMerge(task.from);
            next.m_state->checkOwners(task.from);
            applied = true;
            incompleteEffect = true;
            remove(task.from, *proof, cancel, revisions);
            next.removedMerge(task.from);
            reply.results.emplace_back(task.from, TransferOutcome::Moved);
            incompleteEffect = false;
          } else {
            if (!exists(*task.to.parent(), cancel) || exists(task.to, cancel)) {
              continue;
            }
            for (const auto& binding : bindings) {
              validate(binding, cancel);
            }
            if (task.event.kind == EventKind::RemovedDirectory) {
              next.m_state->bind(*task.to.parent());
              makeDirectory(task.to, cancel);
              applied = true;
              active = task.to;
              incompleteEffect = true;
              setDirectoryMode(task.to, 0700, cancel);
              Proof proof;
              if (task.to.isNative()) {
                const auto parent =
                    checkedFd(::open(task.to.parent()->nativePath()->c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
                proof = nativeProof(parent.get(), *task.to.basename(), next.m_state->budget.entries, 0, cancel);
              } else {
                proof = remoteProof(ph::capture(task.to, next.m_state->budget, cancel));
              }
              next.m_state->edit(task.to, std::move(proof));
              next.m_state->events.push_back(
                  {EventKind::CreatedDirectory, task.to, task.to, true, task.event.mode, task.event.metadata}
              );
              created.emplace_back(task.to, task.event);
              reply.results.emplace_back(task.to, TransferOutcome::Moved);
              incompleteEffect = false;
            } else {
              next.m_state->checkOwners(task.from);
              auto results = runHistoryTransfer(task.from, task.to, TransferAction::Move, next, cancel, bytes);
              applied |= std::ranges::any_of(results, [](const auto& result) {
                return result.outcome == TransferOutcome::Moved || result.outcome == TransferOutcome::Partial;
              });
              reply.results.insert(reply.results.end(), results.begin(), results.end());
              if (!std::ranges::all_of(results, complete)) {
                throw FileErrorException(
                    results.front().error.value_or(FileError{FileErrorKind::Other, "Move replay did not complete."})
                );
              }
              if (!next.m_state->collecting) {
                throwFileError(FileErrorKind::Changed, "The moved items could not supply inverse history.");
              }
              if (task.event.directory) {
                for (auto& other : tasks) {
                  if (!other.done && &other != &task && inside(other.from, task.from)) {
                    other.from = relocated(other.from, task.from, task.to);
                  }
                }
              }
            }
          }
          task.done = progressed = true;
          --remaining;
          completed.fetch_add(1);
        }
        if (!progressed) {
          throwFileError(FileErrorKind::Changed, "Move history folders could not be restored without conflicts.");
        }
      }
      for (const auto& [location, event] : std::views::reverse(created)) {
        active = location;
        incompleteEffect = true;
        next.m_state->restoreContainer(location, event);
        incompleteEffect = false;
      }
      next.m_state->finish();
    }
    ++generation;
    reply.inverse = next.m_state;
    return reply;
  } catch (const std::exception&) {
    const auto error = currentFileError();
    reply.retire = applied
        || error.kind == FileErrorKind::Changed
        || error.kind == FileErrorKind::NotDirectory
        || (!copyRedo && error.kind == FileErrorKind::Missing);
    if (reply.retire) {
      ++generation;
    } else {
      // A preflight failure consumes no operation. A newly constructed wrapper keeps the same generation.
      // The caller still owns this State through its current receipt.
    }
    const auto outcome = incompleteEffect        ? TransferOutcome::Partial
        : error.kind == FileErrorKind::Cancelled ? TransferOutcome::Cancelled
                                                 : TransferOutcome::Failed;
    if (reply.results.empty()) {
      for (const auto& event : events) {
        reply.results.emplace_back(undone ? event.source : event.destination, outcome, error);
      }
    } else if (reply.results.back().outcome == TransferOutcome::Failed) {
      reply.results.back().error = error;
      reply.results.back().outcome = outcome;
    } else if (!std::ranges::any_of(reply.results, [](const auto& result) { return result.error.has_value(); })) {
      reply.results.emplace_back(active.value_or(events.front().destination), outcome, error);
    }
    return reply;
  }
}
