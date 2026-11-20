#include "browser/workbench_model.h"

#include "fs/background_job.h"
#include "fs/file_query.h"

#include <algorithm>
#include <glib.h>
#include <optional>
#include <span>
#include <utility>

namespace {
  using luciole::Location;
  using luciole::MeasureTable;
  using Clock = std::chrono::steady_clock;
  using Listings = std::vector<std::vector<luciole::ListedEntry>>;
  using Want = luciole::WorkbenchWalks::Want;

  // Passes of one pump when calls arrive while it runs; each pass settles what the one before changed.
  constexpr int kPumpRounds = 4;
  constexpr std::size_t kLevelEntries = 4096; // entries listed per ancestor level

  double secondsNow() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
  }

  const MeasureTable* tableOf(const luciole::MeasureFind& found) { return found.table.get(); }

  // What a build or a finish hands back: the draft when trees were built, and the snapshot unless it is unchanged.
  struct Built {
    std::optional<luciole::WorkbenchDraft> draft;
    std::shared_ptr<const luciole::WorkbenchSnapshot> snapshot;
    bool unknown = false; // a build that found nothing of the folder yet; it does not delay the next one
  };

  // Names and kinds of each folder's direct entries, nearest first, at most kLevelEntries each; a folder that
  // cannot be listed gives none. Blocking, for a worker.
  Listings listLevels(const std::vector<Location>& folders, const luciole::CancelToken& token) {
    Listings result(folders.size());
    for (std::size_t i = 0; i < folders.size() && !token.cancelled(); ++i) {
      auto& entries = result[i];
      const auto error = luciole::enumerateChildren(folders[i], token, [&entries](const luciole::ChildInfo& child) {
        entries.push_back({
            .name = child.name,
            .folder = child.type == luciole::FileType::Directory,
            .hidden = child.name.starts_with('.'),
        });
        return entries.size() < kLevelEntries;
      });
      if (error) {
        entries.clear();
      }
    }
    return result;
  }
} // namespace

struct luciole::WorkbenchModel::State : std::enable_shared_from_this<State> {
  State(MeasureCache& measureCache, WorkbenchModelOptions modelOptions)
      : cache(measureCache), options(modelOptions), walks(measureCache, [this] {
          if (const auto self = weak_from_this().lock()) {
            self->pump();
          }
        }) {}

  MeasureCache& cache;
  WorkbenchModelOptions options;
  WorkbenchWalks walks;
  std::uint64_t listener = 0;
  std::optional<Location> location;
  // The ancestors, nearest first, with their listings once read; shared with builds, never changed in place.
  std::shared_ptr<const std::vector<MeasuredLevel>> levels = std::make_shared<const std::vector<MeasuredLevel>>();
  CancelToken listing;
  bool listingPending = false;
  CancelToken work; // the folder's builds; a queued one for a folder left behind never runs
  FolderChanges changes{kPublishInterval};
  workbench::LayoutSettings settings;
  WorkbenchVisits visits;
  std::shared_ptr<const std::vector<VisitRecord>> visitRecords;
  std::shared_ptr<const WorkbenchSnapshot> published;
  std::optional<WorkbenchDraft> draft;        // the last build, finished again when only settings or visits change
  std::vector<const MeasureTable*> builtFrom; // the tables of the last build
  bool rebuild = false;
  bool refinish = false;
  bool building = false;
  bool detached = false;        // the model is gone: nothing more starts
  std::uint64_t generation = 0; // grows with each folder, so a build for a folder left behind is dropped
  GSource* timer = nullptr;
  Clock::time_point lastBuild;
  bool wasMeasuring = false;
  bool pumping = false;
  bool pumpAgain = false;
  std::function<void()> changed;

  ~State() { stopTimer(); }
  State(const State&) = delete;
  State& operator=(const State&) = delete;

  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }

  void stopTimer() {
    if (auto* source = std::exchange(timer, nullptr)) {
      g_source_destroy(source);
      g_source_unref(source);
    }
  }

  bool measuringNow() const { return rebuild || refinish || building || listingPending || walks.waiting(); }

  // Runs after the cache changed, a walk was asked for, a listing arrived or a build finished. A call made while
  // one runs (the cache notifies from invalidate()) is folded into it.
  void pump() {
    if (detached) {
      return;
    }
    if (pumping) {
      pumpAgain = true;
      return;
    }
    pumping = true;
    for (int round = 0; round < kPumpRounds; ++round) {
      pumpAgain = false;
      walks.update();
      if (!pumpAgain) {
        break;
      }
    }
    pumping = false;
    if (location && tables() != builtFrom) {
      rebuild = true;
    }
    schedule();
    const bool now = measuringNow();
    if (now != wasMeasuring) {
      wasMeasuring = now;
      notify();
    }
  }

  // The tables the picture is built from: the folder's, each refreshed child's and each ancestor's.
  std::vector<const MeasureTable*> tables() const {
    std::vector<const MeasureTable*> result{tableOf(cache.find(*location))};
    for (const auto& walk : walks.walks()) {
      if (walk.want == Want::Child || walk.want == Want::Folder) {
        result.push_back(tableOf(walk.found));
      }
    }
    for (const auto& level : *levels) {
      result.push_back(tableOf(cache.find(level.location)));
    }
    return result;
  }

  void schedule() {
    if ((!rebuild && !refinish) || building || timer != nullptr || !location) {
      return;
    }
    const auto due = lastBuild + kPublishInterval;
    const auto wait = std::max(Clock::duration::zero(), due - Clock::now());
    timer = g_timeout_source_new(static_cast<guint>(std::chrono::ceil<std::chrono::milliseconds>(wait).count()));
    g_source_set_callback(
        timer,
        [](gpointer data) -> gboolean {
          const auto state = static_cast<std::weak_ptr<State>*>(data)->lock();
          if (state && state->timer != nullptr) {
            g_source_unref(std::exchange(state->timer, nullptr));
            if (state->rebuild) {
              state->build();
            } else {
              state->finish();
            }
          }
          return G_SOURCE_REMOVE;
        },
        new std::weak_ptr<State>(weak_from_this()),
        [](gpointer data) { delete static_cast<std::weak_ptr<State>*>(data); }
    );
    g_source_attach(timer, g_main_context_default());
  }

  // Each refreshed child with a table; its levels from the last build go along when that table is unchanged.
  std::vector<WorkbenchChildTable> childTables() const {
    std::vector<WorkbenchChildTable> result;
    for (const auto& walk : walks.walks()) {
      if (walk.want != Want::Child || !walk.found.table) {
        continue;
      }
      WorkbenchChildTable child{.name = walk.name, .location = walk.location, .table = walk.found.table, .built = {}};
      if (draft) {
        const auto last = std::ranges::find(draft->refreshed, walk.name, &WorkbenchRefresh::name);
        if (last != draft->refreshed.end() && last->from == child.table) {
          child.built = last->measured;
        }
      }
      result.push_back(std::move(child));
    }
    return result;
  }

  // Hands the cache's view and the page's inputs to a worker, which builds the trees and finishes the snapshot.
  void build() {
    rebuild = false;
    refinish = false;
    if (detached) {
      return;
    }
    builtFrom = tables();
    const auto* folder = walks.folder();
    const auto* table = folder ? tableOf(folder->found) : nullptr;
    WorkbenchBuildInput input{
        .view = cache.view(),
        .location = *location,
        .ancestors = levels,
        .children = childTables(),
        .error = table ? table->error : std::nullopt,
        .folderFinished = walks.folderFinished(),
        .now = secondsNow(),
    };
    start([input = std::move(input), settings = settings, visits = visitRecords, previous = published] {
      Built result{.draft = buildDraft(input), .snapshot = nullptr};
      result.unknown = !result.draft;
      if (result.draft) {
        result.draft->settings = settings;
        result.draft->visits = visits;
        result.snapshot = finishSnapshot(*result.draft, previous);
      }
      return result;
    });
  }

  // Finishes the last draft on a worker with the current settings and visits.
  void finish() {
    refinish = false;
    if (detached || !draft) {
      return;
    }
    draft->settings = settings;
    draft->visits = visitRecords;
    start([work = *draft, previous = published] {
      return Built{.draft = std::nullopt, .snapshot = finishSnapshot(work, previous)};
    });
  }

  void start(std::move_only_function<Built()> job) {
    building = true;
    const auto before = std::exchange(lastBuild, Clock::now());
    runJob<Built>(
        work, std::move(job),
        [weak = weak_from_this(), serial = generation, finishedTree = walks.folderFinished(),
         before](JobResult<Built> reply) {
          if (const auto state = weak.lock()) {
            if (reply && reply->unknown) {
              state->lastBuild = before;
            }
            state->finished(serial, finishedTree, std::move(reply));
          }
        }
    );
  }

  void finished(std::uint64_t serial, bool finishedTree, JobResult<Built> reply) {
    building = false;
    if (detached) {
      return;
    }
    if (serial == generation) {
      if (!reply) {
        publish(
            std::make_shared<const WorkbenchSnapshot>(WorkbenchSnapshot{
                .serial = (published ? published->serial : 0) + 1,
                .location = *location,
                .settings = settings,
                .tree = {},
                .jump = {},
                .jumpLocations = {},
                .shares = {},
                .total = 0,
                .error = reply.error(),
            })
        );
      } else {
        if (reply->draft) {
          draft = std::move(reply->draft);
        }
        if (reply->snapshot) {
          publish(std::move(reply->snapshot));
        }
      }
      if (finishedTree && !walks.jumpsPlanned() && published) {
        walks.planJumps(published->jumpLocations, kJumpWalks);
      }
    }
    pump();
  }

  void publish(std::shared_ptr<const WorkbenchSnapshot> snapshot) {
    published = std::move(snapshot);
    notify();
  }

  void readVisits() {
    visitRecords.reset();
    if (visits && location) {
      const auto& top = levels->empty() ? *location : levels->back().location;
      visitRecords = std::make_shared<const std::vector<VisitRecord>>(visits(top));
    }
  }

  // Lists the ancestors on a worker, so levels the cache does not know yet show their entries.
  void listAncestors(std::vector<Location> folders) {
    listingPending = !folders.empty();
    if (!listingPending) {
      return;
    }
    runJob<Listings>(
        listing, [folders = std::move(folders), token = listing] { return listLevels(folders, token); },
        [weak = weak_from_this(), serial = generation](JobResult<Listings> reply) {
          const auto state = weak.lock();
          if (state && !state->detached && serial == state->generation) {
            state->listed(std::move(reply));
          }
        }
    );
  }

  // A listing that failed leaves the levels without entries.
  void listed(JobResult<Listings> reply) {
    listingPending = false;
    auto next = *levels;
    for (std::size_t i = 0; reply && i < next.size() && i < reply->size(); ++i) {
      next[i].entries = std::move((*reply)[i]);
    }
    levels = std::make_shared<const std::vector<MeasuredLevel>>(std::move(next));
    rebuild = true;
    pump();
  }

  void enter(const Location& folder) {
    listing.cancel();
    listing = CancelToken();
    work.cancel();
    work = CancelToken();
    draft.reset();
    builtFrom.clear();
    ++generation;
    location = folder;
    std::vector<Location> ancestors;
    std::vector<MeasuredLevel> chain;
    auto parent = folder.parent();
    for (std::size_t up = 0; parent && up < std::min(options.ancestorLevels, kMaxAncestors); ++up) {
      chain.push_back({.location = *parent, .entries = {}});
      ancestors.push_back(*parent);
      parent = parent->parent();
    }
    levels = std::make_shared<const std::vector<MeasuredLevel>>(std::move(chain));
    walks.enter(folder, ancestors);
    changes.watch(folder);
    readVisits();
    listAncestors(std::move(ancestors));
    rebuild = true;
  }

  // The folder itself changed on disk, or too many of its entries to walk each alone.
  void folderChanged() {
    if (detached || !location) {
      return;
    }
    walks.walkFolderAgain();
    pump();
  }

  // A file child walks the whole folder again; the walks decide for the rest.
  void invalidate(const std::string& name) {
    if (const auto& shown = published; shown && shown->location == *location) {
      const auto id = shown->node(name);
      if (id != workbench::kNoNode && !shown->tree.nodes[id].folder) {
        walks.walkFolderAgain();
        return;
      }
    }
    walks.refreshChild(name);
  }
};

luciole::WorkbenchModel::WorkbenchModel(MeasureCache& cache, WorkbenchModelOptions options)
    : m_state(std::make_shared<State>(cache, options)) {
  m_state->listener = cache.addListener([weak = std::weak_ptr<State>(m_state)] {
    if (const auto state = weak.lock()) {
      state->pump();
    }
  });
  // An owner may destroy the model from its changed callback, which marks the state detached.
  m_state->changes.setChangedCallback(
      [this, weak = std::weak_ptr<State>(m_state)](std::span<const std::string> names, bool whole) {
        const auto state = weak.lock();
        if (state && whole) {
          state->folderChanged();
        }
        for (const auto& name : names) {
          if (!state || state->detached) {
            return;
          }
          invalidateChild(name);
        }
      }
  );
}

luciole::WorkbenchModel::~WorkbenchModel() {
  m_state->detached = true;
  m_state->listing.cancel();
  m_state->work.cancel();
  m_state->changes.stop();
  m_state->cache.removeListener(m_state->listener);
  m_state->changed = {};
  m_state->stopTimer();
}

void luciole::WorkbenchModel::setLocation(const Location& location) {
  auto& state = *m_state;
  if (state.location == location) {
    return;
  }
  state.enter(location);
  state.pump();
}

void luciole::WorkbenchModel::setSettings(const workbench::LayoutSettings& settings) {
  auto& state = *m_state;
  if (state.settings == settings) {
    return;
  }
  state.settings = settings;
  state.refinish = state.draft.has_value();
  state.schedule();
}

void luciole::WorkbenchModel::setVisits(WorkbenchVisits visits) {
  auto& state = *m_state;
  state.visits = std::move(visits);
  state.readVisits();
  state.refinish = state.draft.has_value();
  state.schedule();
}

void luciole::WorkbenchModel::releaseLocation(const Location& root) { m_state->walks.release(root); }

void luciole::WorkbenchModel::invalidateChild(const std::string& name) {
  auto& state = *m_state;
  if (!state.location || name.empty()) {
    return;
  }
  state.invalidate(name);
  state.pump();
}

std::shared_ptr<const luciole::WorkbenchSnapshot> luciole::WorkbenchModel::snapshot() const {
  return m_state->published;
}

std::uint64_t luciole::WorkbenchModel::serial() const { return m_state->published ? m_state->published->serial : 0; }

bool luciole::WorkbenchModel::measuring() const { return m_state->measuringNow(); }

void luciole::WorkbenchModel::setChangedCallback(std::function<void()> changed) {
  m_state->changed = std::move(changed);
}
